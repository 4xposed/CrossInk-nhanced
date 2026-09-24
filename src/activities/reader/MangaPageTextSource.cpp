#include "MangaPageTextSource.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
struct Decoded {
  uint32_t cp;
  size_t bytes;
  bool valid;
};
Decoded decode(std::string_view text) {
  const auto b = static_cast<uint8_t>(text[0]);
  if (b < 0x80) return {b, 1, true};
  const size_t n = b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
  if (!n || text.size() < n) return {0xfffd, 1, false};
  uint32_t cp = b & (0x7f >> n);
  for (size_t i = 1; i < n; ++i) {
    const auto c = static_cast<uint8_t>(text[i]);
    if ((c & 0xc0) != 0x80) return {0xfffd, 1, false};
    cp = (cp << 6) | (c & 0x3f);
  }
  if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
      (cp >= 0xd800 && cp <= 0xdfff))
    return {0xfffd, 1, false};
  return {cp, n, true};
}
bool space(uint32_t cp) {
  return cp == 0x20 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f ||
         cp == 0x3000;
}
bool japaneseGlyph(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
         (cp >= 0x20000 && cp <= 0x323af);
}
bool hard(const Decoded& d) {
  return !d.valid || d.cp < 0x20 || (d.cp >= 0x7f && d.cp <= 0x9f) || d.cp == 0x2028 || d.cp == 0x2029;
}
struct Item {
  PageTextGlyph glyph;
  manga::format::Rect box;
  std::string_view block;
  size_t offset = 0, length = 0, runOffset = 0;
  int region = 0;
};
// Bubble-only OCR has no glyph boxes. Estimate equal cells in source coordinates
// before the existing crop/rotation transform. State is reused across the walk;
// each line is counted once and no additional glyph array is allocated.
struct ApproximateBlockGeometry {
  int region = -1;
  uint32_t lines = 1, line = 0, column = 0, columns = 1;
  bool vertical = false;

  static uint32_t lineLength(std::string_view text) {
    uint32_t count = 0;
    while (!text.empty()) {
      const auto d = decode(text);
      if (hard(d)) break;
      ++count;
      text.remove_prefix(d.bytes);
    }
    return std::max<uint32_t>(1, count);
  }
  manga::format::Rect next(const Item& item) {
    if (item.block.empty()) return {};
    if (region != item.region) {
      region = item.region;
      lines = 1;
      line = column = 0;
      bool japanese = false;
      auto text = item.block;
      // remove_prefix() shrinks this view on every iteration; cppcheck misses that mutation.
      // cppcheck-suppress knownConditionTrueFalse
      while (!text.empty()) {
        const auto d = decode(text);
        if (hard(d)) ++lines;
        japanese = japanese || japaneseGlyph(d.cp);
        text.remove_prefix(d.bytes);
      }
      vertical = japanese && item.box.h > item.box.w;
      columns = lineLength(item.block);
    }
    if (hard(decode(item.block.substr(item.offset)))) {
      ++line;
      column = 0;
      columns = lineLength(item.block.substr(item.offset + item.length));
      return {};
    }
    const auto& b = item.box;
    const uint32_t x0 = vertical ? b.w * (lines - 1 - line) / lines : b.w * column / columns;
    const uint32_t x1 = vertical ? b.w * (lines - line) / lines : b.w * (column + 1) / columns;
    const uint32_t y0 = vertical ? b.h * column / columns : b.h * line / lines;
    const uint32_t y1 = vertical ? b.h * (column + 1) / columns : b.h * (line + 1) / lines;
    ++column;
    if (!b.w || !b.h || b.x + x0 > UINT16_MAX || b.y + y0 > UINT16_MAX) return {};
    return {uint16_t(b.x + x0), uint16_t(b.y + y0), uint16_t(std::max<uint32_t>(1, x1 - x0)),
            uint16_t(std::max<uint32_t>(1, y1 - y0))};
  }
};
// One ordinal/boundary policy for building and reconstruction; no temporary text
// allocations, no strlen on borrowed format bytes. Return false for malformed views.
template <class Sink>
bool walk(const manga::format::PageView& page, int scope, Sink sink) {
  if (scope < -1 || (scope >= 0 && scope >= page.panels.remaining)) return false;
  uint16_t paragraph = 0, nextWord = 0;
  int region = 0;
  auto panels = page.panels;
  for (int panelIndex = 0; panels.remaining; ++panelIndex) {
    manga::format::PanelView panel;
    if (panels.next(panel) != manga::format::Error::None) return false;
    if (scope >= 0 && panelIndex != scope) continue;
    auto texts = panel.texts;
    while (texts.remaining) {
      manga::format::TextView block;
      if (texts.next(block) != manga::format::Error::None) return false;
      bool inRun = false;
      size_t runOffset = 0;
      uint16_t word = 0;
      for (size_t offset = 0; offset < block.text.size();) {
        const Decoded d = decode(block.text.substr(offset));
        const bool boundary = hard(d), separator = boundary || space(d.cp) || d.cp == 0x2013 || d.cp == 0x2014;
        if (separator) {
          inRun = false;
          runOffset = 0;
        } else if (!inRun) {
          word = nextWord++;
          inRun = true;
          runOffset = 0;
        }
        Item item;
        item.region = region;
        item.glyph.codepoint = d.valid ? d.cp : 0xfffd;
        item.glyph.paragraph = paragraph;
        item.glyph.pageWord = separator ? PageTextGlyph::kSyntheticPageWord : word;
        item.box = block.box;
        item.block = block.text;
        item.offset = offset;
        item.length = d.bytes;
        item.runOffset = runOffset;
        sink(item);
        if (boundary) ++paragraph;
        if (!separator) runOffset += d.bytes;
        offset += d.bytes;
      }
      Item end;
      end.region = region++;
      end.glyph.codepoint = '\n';
      end.glyph.paragraph = paragraph++;
      sink(end);
    }
  }
  return true;
}
bool validRect(const manga::ImageViewport& r) {
  return r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 && int64_t(r.x) + r.width <= INT16_MAX &&
         int64_t(r.y) + r.height <= INT16_MAX;
}
void hashValue(uint32_t& hash, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    hash = (hash ^ (value & 255)) * 16777619u;
    value >>= 8;
  }
}
}  // namespace

bool mapMangaLookupBlock(manga::format::Rect box, const MangaLookupGeometry& g, PageTextBounds& out) {
  out = {};
  const auto& fit = g.layout.geometry;
  if (g.textOnly || g.sourceWidth <= 0 || g.sourceHeight <= 0 || g.sourceWidth > UINT16_MAX ||
      g.sourceHeight > UINT16_MAX || !validRect(g.views.base) || !validRect({fit.x, fit.y, fit.width, fit.height}) ||
      g.views.orientation < 0 || g.views.orientation > 3 || g.layout.orientation < 0 || g.layout.orientation > 3 ||
      g.layout.screenWidth <= 0 || g.layout.screenWidth > INT16_MAX || g.layout.screenHeight <= 0 ||
      g.layout.screenHeight > INT16_MAX)
    return false;
  const int delta = (g.layout.orientation - g.views.orientation + 4) % 4;
  if (g.views.screenWidth != (delta % 2 ? g.layout.screenHeight : g.layout.screenWidth) ||
      g.views.screenHeight != (delta % 2 ? g.layout.screenWidth : g.layout.screenHeight))
    return false;
  const bool cropped = g.sourceCrop.w != 0 || g.sourceCrop.h != 0;
  if (cropped && (g.sourceCrop.w == 0 || g.sourceCrop.h == 0)) return false;
  const int cropX = cropped ? g.sourceCrop.x : 0, cropY = cropped ? g.sourceCrop.y : 0;
  const int sourceWidth = cropped ? g.sourceCrop.w : g.sourceWidth;
  const int sourceHeight = cropped ? g.sourceCrop.h : g.sourceHeight;
  if (cropX + sourceWidth > g.sourceWidth || cropY + sourceHeight > g.sourceHeight) return false;
  int64_t l = std::max<int64_t>(box.x, cropX), t = std::max<int64_t>(box.y, cropY);
  int64_t r = std::min<int64_t>(int64_t(box.x) + box.w, cropX + sourceWidth);
  int64_t b = std::min<int64_t>(int64_t(box.y) + box.h, cropY + sourceHeight);
  if (l >= r || t >= b) return false;
  l = fit.x + (l - cropX) * fit.width / sourceWidth;
  t = fit.y + (t - cropY) * fit.height / sourceHeight;
  r = fit.x + ((r - cropX) * fit.width + sourceWidth - 1) / sourceWidth;
  b = fit.y + ((b - cropY) * fit.height + sourceHeight - 1) / sourceHeight;
  int64_t w = g.layout.screenWidth, h = g.layout.screenHeight;
  for (int i = 0; i < delta; ++i) {
    const int64_t nl = t, nt = w - r, nr = b, nb = w - l;
    l = nl;
    t = nt;
    r = nr;
    b = nb;
    std::swap(w, h);
  }
  l = std::max<int64_t>(l, g.views.base.x);
  t = std::max<int64_t>(t, g.views.base.y);
  r = std::min<int64_t>(r, int64_t(g.views.base.x) + g.views.base.width);
  b = std::min<int64_t>(b, int64_t(g.views.base.y) + g.views.base.height);
  if (l >= r || t >= b || l < 0 || t < 0 || r > INT16_MAX || b > INT16_MAX) return false;
  out = {int16_t(l), int16_t(t), int16_t(r - l), int16_t(b - t)};
  return true;
}

namespace {
template <class Sink>
bool visitRegions(const manga::format::PageView& page, int panel, const MangaLookupGeometry& geometry, Sink sink) {
  int previous = -1;
  return walk(page, panel, [&](const Item& item) {
    if (item.region == previous || item.glyph.pageWord == PageTextGlyph::kSyntheticPageWord) return;
    previous = item.region;
    PageTextBounds bounds;
    if (mapMangaLookupBlock(item.box, geometry, bounds)) sink(item.region, bounds);
  });
}
}  // namespace

bool mangaLookupRegionBounds(const manga::format::PageView& page, int panel, const MangaLookupGeometry& geometry,
                             int region, PageTextBounds& out) {
  out = {};
  const bool valid = visitRegions(page, panel, geometry, [&](int index, const PageTextBounds& bounds) {
    if (index == region) out = bounds;
  });
  if (!valid) out = {};
  return out.width > 0 && out.height > 0;
}

int nextMangaLookupRegion(const manga::format::PageView& page, int panel, const MangaLookupGeometry& geometry,
                          int current, bool forward) {
  int first = -1, last = -1, before = -1, after = -1;
  if (!visitRegions(page, panel, geometry, [&](int index, const PageTextBounds&) {
        if (first < 0) first = index;
        last = index;
        if (index < current) before = index;
        if (index > current && after < 0) after = index;
      }))
    return -1;
  return forward ? (after >= 0 ? after : first) : (before >= 0 ? before : last);
}

int mangaLookupRegionAtPoint(const manga::format::PageView& page, int panel, const MangaLookupGeometry& geometry, int x,
                             int y) {
  int selected = -1;
  int32_t area = INT32_MAX;
  if (!visitRegions(page, panel, geometry, [&](int index, const PageTextBounds& b) {
        const int32_t candidateArea = int32_t(b.width) * b.height;
        if (x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height && candidateArea < area) {
          selected = index;
          area = candidateArea;
        }
      }))
    return -1;
  return selected;
}

bool mangaLookupRegionHasSingleToken(const manga::format::PageView& page, const int panel, const int region) {
  uint16_t word = PageTextGlyph::kSyntheticPageWord;
  size_t glyphCount = 0;
  bool multipleWords = false, japanese = false;
  const bool valid = walk(page, panel, [&](const Item& item) {
    if (item.region != region || item.glyph.pageWord == PageTextGlyph::kSyntheticPageWord) return;
    if (word != PageTextGlyph::kSyntheticPageWord && word != item.glyph.pageWord) multipleWords = true;
    word = item.glyph.pageWord;
    japanese = japanese || japaneseGlyph(item.glyph.codepoint);
    ++glyphCount;
  });
  // OCR stores block rectangles, not character boxes. A Japanese lexical run
  // can contain several dictionary words without spaces, so it is ambiguous.
  return valid && glyphCount != 0 && !multipleWords && (!japanese || glyphCount == 1);
}

DictionaryStatus buildMangaLookupTextSource(const manga::format::PageView& page, int panel,
                                            const MangaLookupGeometry& geometry, OwnedLookupTextSource& out,
                                            const int region, const MangaTextMeasure measure) {
  out.clear();
  if (geometry.textOnly &&
      (!validRect(geometry.views.base) || geometry.cellWidth <= 0 || geometry.lineHeight <= 0 ||
       geometry.cellWidth > geometry.views.base.width || geometry.lineHeight > geometry.views.base.height)) {
    LOG_ERR("MLO", "Invalid text fallback geometry");
    return DictionaryStatus::ReadError;
  }
  PageTextBounds fullBounds;
  if (!geometry.textOnly &&
      (geometry.sourceWidth <= 0 || geometry.sourceWidth > UINT16_MAX || geometry.sourceHeight <= 0 ||
       geometry.sourceHeight > UINT16_MAX ||
       !mapMangaLookupBlock({0, 0, uint16_t(geometry.sourceWidth), uint16_t(geometry.sourceHeight)}, geometry,
                            fullBounds))) {
    LOG_ERR("MLO", "Invalid full-page OCR geometry");
    return DictionaryStatus::ReadError;
  }
  size_t count = 0;
  if (region < -1 || !walk(
                         page, panel,
                         [&](const Item& item) {
                           if (region < 0 || item.region == region) ++count;
                         })) {
    LOG_ERR("MLO", "Invalid OCR page/scope");
    return DictionaryStatus::ReadError;
  }
  if (!count) return DictionaryStatus::NotFound;
  size_t capacity = std::min<size_t>(count, kMangaLookupMaxGlyphs);
  // One activity-lifetime array, at most 16KiB; too large for task stack and
  // dynamic page ownership excludes static storage. Scanner adds at most 8KiB.
  auto glyphs = makeUniqueNoThrow<PageTextGlyph[]>(capacity);
  if (!glyphs) {
    LOG_ERR("MLO", "OOM allocating %u OCR glyphs", unsigned(capacity));
    if (capacity > 256) {
      capacity = 256;
      glyphs = makeUniqueNoThrow<PageTextGlyph[]>(capacity);
    }
    if (!glyphs) {
      LOG_ERR("MLO", "OCR glyph allocation failed");
      return DictionaryStatus::OutOfMemory;
    }
  }
  size_t used = 0, complete = 0;
  bool stopped = false;
  int column = 0, row = 0, textX = 0;
  const int columns = geometry.textOnly ? geometry.views.base.width / geometry.cellWidth : 0;
  const int textHeight = region >= 0 ? INT16_MAX - geometry.views.base.y : geometry.views.base.height;
  const int rows = geometry.textOnly ? textHeight / geometry.lineHeight : 0;
  uint16_t previousWord = PageTextGlyph::kSyntheticPageWord;
  ApproximateBlockGeometry approximation;
  walk(page, panel, [&](const Item& item) {
    if (region >= 0 && item.region != region) return;
    if (stopped) return;
    if (item.glyph.pageWord != previousWord || item.glyph.pageWord == PageTextGlyph::kSyntheticPageWord)
      complete = used;
    previousWord = item.glyph.pageWord;
    if (used == capacity) {
      stopped = true;
      return;
    }
    PageTextGlyph glyph = item.glyph;
    PageTextBounds bounds;
    const auto estimated = geometry.approximateTextPositions ? approximation.next(item) : item.box;
    if (geometry.textOnly && measure.advance) {
      const bool separator = glyph.pageWord == PageTextGlyph::kSyntheticPageWord;
      if (separator && hard({glyph.codepoint, 1, glyph.codepoint != 0xfffd})) {
        textX = 0;
        ++row;
      } else {
        const int advance = std::clamp(measure.advance(measure.context, glyph.codepoint), 1, geometry.views.base.width);
        if (textX + advance > geometry.views.base.width) {
          textX = 0;
          ++row;
          // A wrapping space is not indentation on the next line.
          if (separator) {
            glyphs[used++] = glyph;
            return;
          }
        }
        if (!separator) {
          if (row >= rows) {
            stopped = true;
            return;
          }
          bounds = {int16_t(geometry.views.base.x + textX), int16_t(geometry.views.base.y + row * geometry.lineHeight),
                    int16_t(advance), int16_t(geometry.lineHeight)};
        }
        textX += advance;
      }
    } else if (geometry.textOnly) {
      if (glyph.pageWord == PageTextGlyph::kSyntheticPageWord) {
        if (hard({glyph.codepoint, 1, glyph.codepoint != 0xfffd})) {
          column = 0;
          ++row;
        } else if (++column >= columns) {
          column = 0;
          ++row;
        }
      } else {
        if (row >= rows) {
          stopped = true;
          return;
        }
        bounds = {int16_t(geometry.views.base.x + column * geometry.cellWidth),
                  int16_t(geometry.views.base.y + row * geometry.lineHeight), int16_t(geometry.cellWidth),
                  int16_t(geometry.lineHeight)};
        if (++column >= columns) {
          column = 0;
          ++row;
        }
      }
    } else if (glyph.pageWord != PageTextGlyph::kSyntheticPageWord) {
      // Keep text/ordinal identity even if its box is entirely clipped. An
      // invisible block must not acquire a fabricated selectable rectangle.
      mapMangaLookupBlock(estimated, geometry, bounds);
    }
    glyph.x = bounds.x;
    glyph.y = bounds.y;
    glyph.width = bounds.width;
    glyph.height = bounds.height;
    glyphs[used++] = glyph;
    // Japanese scanning segments inside a lexical run. Preserve its complete
    // codepoint prefix, while withholding any trailing unfinished Latin token.
    if (japaneseGlyph(glyph.codepoint)) complete = used;
  });
  if (stopped) {
    used = complete;
    out.truncated = true;
    LOG_ERR("MLO", "OCR source truncated at complete lexical run (%u glyphs)", unsigned(used));
  }
  if (!used) return out.truncated ? DictionaryStatus::OutOfMemory : DictionaryStatus::NotFound;
  uint32_t hash = 2166136261u;
  hashValue(hash, 0x4d4f0001);
  hashValue(hash, uint32_t(panel));
  if (region >= 0) hashValue(hash, uint32_t(region) ^ 0x52454700u);
  for (size_t i = 0; i < used; ++i) {
    hashValue(hash, glyphs[i].codepoint);
    hashValue(hash, glyphs[i].paragraph);
    hashValue(hash, glyphs[i].pageWord);
  }
  out.glyphs = std::move(glyphs);
  out.glyphCount = uint16_t(used);
  out.contentHash = hash;
  return DictionaryStatus::Found;
}

bool copyMangaLookupClipping(const manga::format::PageView& page, int panel, MangaLookupClippingRange range, char* out,
                             size_t capacity, size_t& written) {
  written = 0;
  if (out && capacity) out[0] = 0;
  if (!out || !capacity || range.firstPageWordOrdinal > range.lastPageWordOrdinal ||
      range.lastPageWordOrdinal == PageTextGlyph::kSyntheticPageWord)
    return false;
  std::string_view block;
  size_t begin = 0, end = 0;
  uint16_t paragraph = 0;
  bool foundBegin = false, foundEnd = false;
  const bool valid = walk(page, panel, [&](const Item& item) {
    if (item.glyph.pageWord == PageTextGlyph::kSyntheticPageWord) return;
    if (item.glyph.pageWord == range.firstPageWordOrdinal && item.runOffset == range.firstWordByteOffset) {
      block = item.block;
      begin = item.offset;
      paragraph = item.glyph.paragraph;
      foundBegin = true;
    }
    if (item.glyph.pageWord == range.lastPageWordOrdinal &&
        item.runOffset + item.length == range.lastWordByteEndOffset && foundBegin &&
        item.block.data() == block.data() && item.glyph.paragraph == paragraph) {
      end = item.offset + item.length;
      foundEnd = true;
    }
  });
  if (!valid || !foundBegin || !foundEnd || end <= begin || end - begin >= capacity) return false;
  written = end - begin;
  std::memcpy(out, block.data() + begin, written);
  out[written] = 0;
  return true;
}

bool mangaLookupCacheFileName(uint32_t physicalPage, int panel, char* out, size_t capacity) {
  if (!out || !capacity) return false;
  out[0] = 0;
  if (physicalPage >= manga::format::kMaxPages || panel < -1 || panel > 254) return false;
  const int n =
      panel < 0 ? std::snprintf(out, capacity, "manga-ocr-%u-overview.bin", unsigned(physicalPage))
                : std::snprintf(out, capacity, "manga-ocr-%u-panel-%u.bin", unsigned(physicalPage), unsigned(panel));
  if (n < 0 || size_t(n) >= capacity) {
    out[0] = 0;
    return false;
  }
  return true;
}
