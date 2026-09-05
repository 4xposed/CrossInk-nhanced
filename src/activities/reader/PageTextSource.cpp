#include "PageTextSource.h"

#include <BidiUtils.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

namespace {

constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr uint16_t FALLBACK_GLYPH_CAPACITY = 256;
constexpr uint32_t FNV1A_OFFSET = 2166136261U;
constexpr uint32_t FNV1A_PRIME = 16777619U;

struct DecodedCodepoint {
  uint32_t value = 0;
  size_t bytes = 0;
};

bool decodeUtf8(const char* text, const size_t length, const size_t offset, DecodedCodepoint& out) {
  if (!text || offset >= length) return false;
  const auto* bytes = reinterpret_cast<const uint8_t*>(text);
  const uint8_t lead = bytes[offset];
  if (lead == 0) return false;
  if (lead < 0x80) {
    out = {lead, 1};
    return true;
  }

  size_t count = 0;
  uint32_t codepoint = 0;
  uint32_t minimum = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    count = 2;
    codepoint = lead & 0x1FU;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    count = 3;
    codepoint = lead & 0x0FU;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    count = 4;
    codepoint = lead & 0x07U;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (count > length - offset) return false;
  for (size_t index = 1; index < count; ++index) {
    const uint8_t continuation = bytes[offset + index];
    if ((continuation & 0xC0U) != 0x80U) return false;
    codepoint = (codepoint << 6U) | (continuation & 0x3FU);
  }
  if (codepoint < minimum || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return false;
  out = {codepoint, count};
  return true;
}

bool validateUtf8(const char* text, const size_t length) {
  size_t offset = 0;
  while (offset < length) {
    DecodedCodepoint decoded;
    if (!decodeUtf8(text, length, offset, decoded)) return false;
    offset += decoded.bytes;
  }
  return true;
}

bool isDashSeparator(const char* text, const size_t length, const size_t offset) {
  return offset + 2 < length && static_cast<uint8_t>(text[offset]) == 0xE2U &&
         static_cast<uint8_t>(text[offset + 1]) == 0x80U &&
         (static_cast<uint8_t>(text[offset + 2]) == 0x93U || static_cast<uint8_t>(text[offset + 2]) == 0x94U);
}

bool containsDashSeparator(const char* text, const size_t length) {
  for (size_t offset = 0; offset < length; ++offset) {
    if (isDashSeparator(text, length, offset)) return true;
  }
  return false;
}

bool hasVisibleWordText(const char* text) {
  if (!text) return false;
  const char* cursor = text;
  if (static_cast<uint8_t>(cursor[0]) == 0xE2U && cursor[1] != '\0' && cursor[2] != '\0' &&
      static_cast<uint8_t>(cursor[1]) == 0x80U && static_cast<uint8_t>(cursor[2]) == 0x83U) {
    cursor += 3;
  }
  while (*cursor) {
    if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') return true;
    ++cursor;
  }
  return false;
}

bool containsSoftHyphen(const char* text, const size_t length) {
  if (!text) return false;
  for (size_t offset = 0; offset + 1 < length; ++offset) {
    if (text[offset] == SOFT_HYPHEN_UTF8[0] && text[offset + 1] == SOFT_HYPHEN_UTF8[1]) return true;
  }
  return false;
}

const char* withoutSoftHyphens(const char* text, const size_t length, char* scratch, const size_t scratchCapacity) {
  if (!text || !scratch || scratchCapacity == 0) return text;
  if (!containsSoftHyphen(text, length) || length + 1 > scratchCapacity) return text;

  size_t used = 0;
  for (size_t offset = 0; offset < length;) {
    if (offset + 1 < length && text[offset] == SOFT_HYPHEN_UTF8[0] && text[offset + 1] == SOFT_HYPHEN_UTF8[1]) {
      offset += SOFT_HYPHEN_BYTES;
      continue;
    }
    scratch[used++] = text[offset++];
  }
  scratch[used] = '\0';
  return scratch;
}

int16_t measureBionicText(const GfxRenderer& renderer, const int fontId, const char* text, const size_t textLength,
                          const EpdFontFamily::Style style, const uint8_t bionicBoundary,
                          const uint16_t bionicRunOffset, const bool isRtl, char* scratch,
                          const size_t scratchCapacity) {
  if (bionicBoundary == 0 || bionicRunOffset == 0) {
    return measureHorizontalPageText(renderer, fontId, text, textLength, style, scratch, scratchCapacity);
  }
  if (!isRtl) {
    const size_t suffixStart = std::min<size_t>(bionicBoundary, textLength);
    return static_cast<int16_t>(bionicRunOffset + renderer.getTextAdvanceX(fontId, text + suffixStart, style));
  }

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  char boldPrefix[40];
  const size_t boldLength =
      std::min<size_t>({static_cast<size_t>(bionicBoundary), textLength, sizeof(boldPrefix) - 1U});
  std::memcpy(boldPrefix, text, boldLength);
  boldPrefix[boldLength] = '\0';
  return static_cast<int16_t>(bionicRunOffset + renderer.getTextAdvanceX(fontId, boldPrefix, boldStyle));
}

bool isAsciiWordCodepoint(const uint32_t codepoint) {
  return (codepoint >= 'A' && codepoint <= 'Z') || (codepoint >= 'a' && codepoint <= 'z') ||
         (codepoint >= '0' && codepoint <= '9');
}

template <typename Sink>
bool forEachLogicalCodepoint(const HorizontalPageWordGeometry& word, Sink&& sink, uint32_t& firstCodepoint,
                             uint32_t& lastCodepoint, uint16_t& codepointCount) {
  firstCodepoint = 0;
  lastCodepoint = 0;
  codepointCount = 0;
  size_t offset = 0;
  while (offset < word.textLength) {
    DecodedCodepoint decoded;
    if (!decodeUtf8(word.text, word.textLength, offset, decoded)) return false;
    const bool insertedTrailingHyphen =
        word.endsWithInsertedHyphen && decoded.value == '-' && offset + decoded.bytes == word.textLength;
    offset += decoded.bytes;
    if (insertedTrailingHyphen) continue;
    if (codepointCount == 0) firstCodepoint = decoded.value;
    lastCodepoint = decoded.value;
    if (codepointCount == UINT16_MAX) return false;
    ++codepointCount;
    sink(decoded.value);
  }
  return true;
}

struct CountContext {
  size_t glyphCount = 0;
  size_t measurementScratchBytes = 0;
  uint32_t previousCodepoint = 0;
  bool previousEndedWithInsertedHyphen = false;
  bool overflow = false;
};

bool countWord(void* context, const HorizontalPageWordGeometry& word) {
  auto& count = *static_cast<CountContext*>(context);
  uint32_t first = 0;
  uint32_t last = 0;
  uint16_t wordCodepoints = 0;
  if (!forEachLogicalCodepoint(word, [](uint32_t) {}, first, last, wordCodepoints)) return false;
  if (wordCodepoints == 0) return true;
  const bool separator = count.previousCodepoint != 0 && !count.previousEndedWithInsertedHyphen &&
                         isAsciiWordCodepoint(count.previousCodepoint) && isAsciiWordCodepoint(first);
  const size_t append = static_cast<size_t>(wordCodepoints) + (separator ? 1U : 0U);
  if (append > std::numeric_limits<size_t>::max() - count.glyphCount) {
    count.overflow = true;
    return false;
  }
  count.glyphCount += append;
  count.previousCodepoint = last;
  count.previousEndedWithInsertedHyphen = word.endsWithInsertedHyphen;
  return true;
}

DictionaryStatus countPageGlyphs(const Page& page, CountContext& count) {
  uint16_t pageWordOrdinal = 0;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    for (uint16_t wordIndex = 0; wordIndex < block->wordCount(); ++wordIndex) {
      const char* text = block->wordText(wordIndex);
      const size_t textLength = block->wordTextLen(wordIndex);
      if (!validateUtf8(text, textLength)) {
        LOG_ERR("PTS", "Invalid UTF-8 in page word %u", static_cast<unsigned>(pageWordOrdinal));
        return DictionaryStatus::ReadError;
      }
      if (!hasVisibleWordText(text)) continue;
      if (containsSoftHyphen(text, textLength)) {
        // TextBlock stores word lengths as uint16_t, so the terminating byte
        // keeps this bounded to 65,536 bytes without size_t overflow.
        count.measurementScratchBytes = std::max(count.measurementScratchBytes, textLength + 1U);
      }
      if (pageWordOrdinal == PageTextGlyph::kSyntheticPageWord) {
        LOG_ERR("PTS", "Page word ordinal exceeds the source format");
        return DictionaryStatus::ReadError;
      }
      HorizontalPageWordGeometry word;
      word.text = text;
      word.textLength = textLength;
      word.pageWord = pageWordOrdinal++;
      word.endsWithInsertedHyphen = block->wordEndsWithInsertedHyphen(wordIndex);
      if (!countWord(&count, word)) {
        LOG_ERR("PTS", "Page glyph count overflow");
        return DictionaryStatus::ReadError;
      }
    }
  }
  return DictionaryStatus::Found;
}

struct PopulateContext {
  PageTextGlyph* glyphs = nullptr;
  uint16_t capacity = 0;
  uint16_t used = 0;
  uint32_t previousCodepoint = 0;
  bool previousEndedWithInsertedHyphen = false;
};

bool appendGlyph(PopulateContext& populate, const uint32_t codepoint, const HorizontalPageWordGeometry* word) {
  if (populate.used >= populate.capacity) return true;
  auto& glyph = populate.glyphs[populate.used++];
  glyph.codepoint = codepoint;
  glyph.paragraph = 0;
  if (word) {
    glyph.pageWord = word->pageWord;
    glyph.x = word->x;
    glyph.y = word->y;
    glyph.width = word->width;
    glyph.height = word->height;
  } else {
    glyph.pageWord = PageTextGlyph::kSyntheticPageWord;
  }
  return true;
}

bool populateWord(void* context, const HorizontalPageWordGeometry& word) {
  auto& populate = *static_cast<PopulateContext*>(context);
  uint32_t first = 0;
  uint32_t last = 0;
  uint16_t wordCodepoints = 0;

  // Decode once to establish the ASCII-boundary decision. UTF-8 was already
  // validated by visitHorizontalPageWords; failure still remains recoverable.
  if (!forEachLogicalCodepoint(word, [](uint32_t) {}, first, last, wordCodepoints)) return false;
  if (wordCodepoints == 0) return true;
  if (populate.previousCodepoint != 0 && !populate.previousEndedWithInsertedHyphen &&
      isAsciiWordCodepoint(populate.previousCodepoint) && isAsciiWordCodepoint(first)) {
    appendGlyph(populate, ' ', nullptr);
  }
  if (!forEachLogicalCodepoint(
          word, [&populate, &word](const uint32_t codepoint) { appendGlyph(populate, codepoint, &word); }, first, last,
          wordCodepoints)) {
    return false;
  }
  populate.previousCodepoint = last;
  populate.previousEndedWithInsertedHyphen = word.endsWithInsertedHyphen;
  return true;
}

void fnvMix32(uint32_t& hash, const uint32_t value) {
  for (uint8_t byte = 0; byte < 4; ++byte) {
    hash ^= static_cast<uint8_t>(value >> (byte * 8U));
    hash *= FNV1A_PRIME;
  }
}

uint32_t hashGlyphs(const PageTextGlyph* glyphs, const uint16_t count) {
  uint32_t hash = FNV1A_OFFSET;
  fnvMix32(hash, count);
  for (uint16_t index = 0; index < count; ++index) {
    const auto& glyph = glyphs[index];
    fnvMix32(hash, glyph.codepoint);
    fnvMix32(hash, glyph.paragraph);
    fnvMix32(hash, glyph.pageWord);
    fnvMix32(hash, static_cast<uint16_t>(glyph.x));
    fnvMix32(hash, static_cast<uint16_t>(glyph.y));
    fnvMix32(hash, static_cast<uint16_t>(glyph.width));
    fnvMix32(hash, static_cast<uint16_t>(glyph.height));
  }
  return hash;
}

}  // namespace

int16_t measureHorizontalPageText(const GfxRenderer& renderer, const int fontId, const char* text,
                                  const size_t textLength, const EpdFontFamily::Style style, char* scratch,
                                  const size_t scratchCapacity) {
  const char* measured = withoutSoftHyphens(text, textLength, scratch, scratchCapacity);
  return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, measured, style));
}

DictionaryStatus visitHorizontalPageWords(const Page& page, GfxRenderer& renderer, const int fontId,
                                          const int marginLeft, const int marginTop, char* measurementScratch,
                                          const size_t scratchCapacity, const HorizontalPageWordSink sink) {
  if (!sink.onWord) {
    LOG_ERR("PTS", "Horizontal page word sink is missing");
    return DictionaryStatus::ReadError;
  }
  const int16_t naturalSpaceWidth = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, " ", EpdFontFamily::REGULAR));
  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId));
  uint16_t pageWordOrdinal = 0;

  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t sourceWordCount = block->wordCount();
    for (uint16_t index = 0; index < sourceWordCount; ++index) {
      if (!validateUtf8(block->wordText(index), block->wordTextLen(index))) {
        LOG_ERR("PTS", "Invalid UTF-8 in page word %u", static_cast<unsigned>(pageWordOrdinal));
        return DictionaryStatus::ReadError;
      }
    }

    const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
    int16_t lineGapWidth = naturalSpaceWidth;
    if (sourceWordCount >= 2 && block->wordTextLen(0) > 0) {
      const char* firstText = block->wordText(0);
      const size_t firstLength = block->wordTextLen(0);
      const auto firstStyle = block->wordStyle(0);
      const bool firstIsRtl = BidiUtils::detectParagraphLevel(firstText, block->getBlockStyle().isRtl ? 1 : 0) == 1;
      const int16_t firstWidth =
          measureBionicText(renderer, fontId, firstText, firstLength, firstStyle, block->bionicBoundary(0),
                            block->bionicRunOffset(0), firstIsRtl, measurementScratch, scratchCapacity);
      const int16_t derivedGap = static_cast<int16_t>(block->wordXpos(1) - block->wordXpos(0) - firstWidth);
      if (derivedGap > naturalSpaceWidth / 2) lineGapWidth = derivedGap;
    }

    int lastSelectableWordIndex = -2;
    HorizontalPageWordGeometry previousSelectable{};
    for (uint16_t wordIndex = 0; wordIndex < sourceWordCount; ++wordIndex) {
      const char* text = block->wordText(wordIndex);
      const size_t textLength = block->wordTextLen(wordIndex);
      if (!hasVisibleWordText(text)) {
        lastSelectableWordIndex = -2;
        continue;
      }
      if (pageWordOrdinal == PageTextGlyph::kSyntheticPageWord) {
        LOG_ERR("PTS", "Page word ordinal exceeds the source format");
        return DictionaryStatus::ReadError;
      }

      HorizontalPageWordGeometry word;
      word.text = text;
      word.textLength = textLength;
      word.sourceWordIndex = wordIndex;
      word.pageWord = pageWordOrdinal++;
      word.x = static_cast<int16_t>(line->xPos + block->wordXpos(wordIndex) + marginLeft);
      word.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      word.height = lineHeight;
      word.style = block->wordStyle(wordIndex);
      word.bionicBoundary = block->bionicBoundary(wordIndex);
      word.bionicRunOffset = block->bionicRunOffset(wordIndex);
      word.isRtl = BidiUtils::detectParagraphLevel(text, block->getBlockStyle().isRtl ? 1 : 0) == 1;
      word.selectable = utf8ContainsLookupCharacter(text);
      word.containsDashSeparator = containsDashSeparator(text, textLength);
      word.endsWithInsertedHyphen = block->wordEndsWithInsertedHyphen(wordIndex);

      if (word.bionicBoundary > 0 && word.bionicRunOffset > 0) {
        word.width = measureBionicText(renderer, fontId, text, textLength, word.style, word.bionicBoundary,
                                       word.bionicRunOffset, word.isRtl, measurementScratch, scratchCapacity);
      } else if (wordIndex + 1 < sourceWordCount) {
        const int16_t raw = static_cast<int16_t>(block->wordXpos(wordIndex + 1) - block->wordXpos(wordIndex));
        word.width = std::max<int16_t>(1, static_cast<int16_t>(raw - lineGapWidth));
      } else {
        word.width = measureHorizontalPageText(renderer, fontId, text, textLength, word.style, measurementScratch,
                                               scratchCapacity);
      }

      if (!word.selectable) {
        lastSelectableWordIndex = -2;
      } else if (!word.containsDashSeparator) {
        if (lastSelectableWordIndex == static_cast<int>(wordIndex) - 1) {
          const uint16_t previousIndex = static_cast<uint16_t>(wordIndex - 1);
          const int16_t previousMeasuredWidth = static_cast<int16_t>(
              renderer.getTextAdvanceX(fontId, block->wordText(previousIndex), block->wordStyle(previousIndex)));
          const int16_t currentMeasuredWidth =
              measureBionicText(renderer, fontId, text, textLength, word.style, word.bionicBoundary,
                                word.bionicRunOffset, word.isRtl, measurementScratch, scratchCapacity);
          const int currentLeft = word.x;
          const int currentRight = word.x + currentMeasuredWidth;
          const int previousLeft = previousSelectable.x;
          const int previousRight = previousLeft + previousMeasuredWidth;
          const int gap = currentLeft >= previousLeft ? currentLeft - previousRight : previousLeft - currentRight;
          word.joinWithoutSpaceBefore = gap < naturalSpaceWidth / 2;
          if (word.joinWithoutSpaceBefore) {
            word.previousJoinedWidth = previousMeasuredWidth;
            word.width = currentMeasuredWidth;
          }
        }
        lastSelectableWordIndex = wordIndex;
        previousSelectable = word;
      } else {
        lastSelectableWordIndex = -2;
      }

      if (!sink.onWord(sink.context, word)) {
        LOG_ERR("PTS", "Horizontal page word sink rejected word %u", static_cast<unsigned>(word.pageWord));
        return DictionaryStatus::OutOfMemory;
      }
    }
  }
  return DictionaryStatus::Found;
}

bool unionPageTextGlyphBounds(const PageTextSourceView source, const uint16_t firstGlyph, const uint16_t glyphCount,
                              PageTextBounds& out) {
  out = {};
  if (!source.glyphs || firstGlyph >= source.glyphCount || glyphCount == 0 ||
      glyphCount > source.glyphCount - firstGlyph) {
    return false;
  }
  bool haveBounds = false;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  for (uint16_t offset = 0; offset < glyphCount; ++offset) {
    const auto& glyph = source.glyphs[firstGlyph + offset];
    if (glyph.pageWord == PageTextGlyph::kSyntheticPageWord || glyph.width <= 0 || glyph.height <= 0) continue;
    const int glyphRight = glyph.x + glyph.width;
    const int glyphBottom = glyph.y + glyph.height;
    if (!haveBounds) {
      left = glyph.x;
      top = glyph.y;
      right = glyphRight;
      bottom = glyphBottom;
      haveBounds = true;
    } else {
      left = std::min(left, static_cast<int>(glyph.x));
      top = std::min(top, static_cast<int>(glyph.y));
      right = std::max(right, glyphRight);
      bottom = std::max(bottom, glyphBottom);
    }
  }
  if (!haveBounds) return false;
  out.x = static_cast<int16_t>(left);
  out.y = static_cast<int16_t>(top);
  out.width = static_cast<int16_t>(right - left);
  out.height = static_cast<int16_t>(bottom - top);
  return true;
}

DictionaryStatus HorizontalPageTextSource::build(const Page& page, GfxRenderer& renderer, const int fontId,
                                                 const int marginLeft, const int marginTop) {
  clear();
  CountContext count;
  DictionaryStatus status = countPageGlyphs(page, count);
  if (status != DictionaryStatus::Found) return status;
  if (count.overflow) {
    LOG_ERR("PTS", "Page glyph count overflow");
    return DictionaryStatus::ReadError;
  }
  if (count.glyphCount == 0) {
    contentHash_ = hashGlyphs(nullptr, 0);
    return DictionaryStatus::Found;
  }

  uint16_t capacity = 0;
  bool usedFallback = count.glyphCount > UINT16_MAX;
  if (!usedFallback) {
    capacity = static_cast<uint16_t>(count.glyphCount);
    // Runtime-sized page metadata cannot live on a small task stack or in
    // static storage. Exact activity-lifetime allocation is at most
    // UINT16_MAX * sizeof(PageTextGlyph), with a 4 KiB bounded fallback.
    glyphs_ = makeUniqueNoThrow<PageTextGlyph[]>(capacity);
    if (!glyphs_) {
      LOG_ERR("PTS", "OOM allocating %u-byte exact page glyph array",
              static_cast<unsigned>(static_cast<size_t>(capacity) * sizeof(PageTextGlyph)));
      renderer.releaseSdCardFontForLowMemory(fontId);
      glyphs_ = makeUniqueNoThrow<PageTextGlyph[]>(capacity);
    }
  }

  if (!glyphs_) {
    capacity = static_cast<uint16_t>(std::min<size_t>(count.glyphCount, FALLBACK_GLYPH_CAPACITY));
    glyphs_ = makeUniqueNoThrow<PageTextGlyph[]>(capacity);
    usedFallback = true;
    if (!glyphs_) {
      LOG_ERR("PTS", "OOM allocating %u-byte bounded page glyph fallback",
              static_cast<unsigned>(static_cast<size_t>(capacity) * sizeof(PageTextGlyph)));
      clear();
      return DictionaryStatus::OutOfMemory;
    }
  }

  std::unique_ptr<char[]> measurementScratch;
  if (count.measurementScratchBytes > 0) {
    // A page word can contain up to UINT16_MAX bytes, so this cannot safely
    // live on the small activity stack. One fallible buffer is reused by every
    // word measurement and released when build() returns.
    measurementScratch = makeUniqueNoThrow<char[]>(count.measurementScratchBytes);
    if (!measurementScratch) {
      LOG_ERR("PTS", "OOM allocating %u-byte soft-hyphen measurement scratch",
              static_cast<unsigned>(count.measurementScratchBytes));
      clear();
      return DictionaryStatus::OutOfMemory;
    }
  }

  PopulateContext populate{glyphs_.get(), capacity};
  status = visitHorizontalPageWords(page, renderer, fontId, marginLeft, marginTop, measurementScratch.get(),
                                    count.measurementScratchBytes, HorizontalPageWordSink{&populate, &populateWord});
  if (status != DictionaryStatus::Found) {
    clear();
    return status;
  }
  glyphCount_ = populate.used;
  truncated_ = usedFallback || count.glyphCount > glyphCount_;
  contentHash_ = hashGlyphs(glyphs_.get(), glyphCount_);
  return DictionaryStatus::Found;
}

PageTextSourceView HorizontalPageTextSource::view() const {
  return PageTextSourceView{glyphs_.get(), glyphCount_, contentHash_};
}

void HorizontalPageTextSource::clear() {
  glyphs_.reset();
  glyphCount_ = 0;
  contentHash_ = 0;
  truncated_ = false;
}
