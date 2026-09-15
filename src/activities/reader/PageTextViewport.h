#pragma once

#include <algorithm>

#include "PageTextSource.h"

inline bool pageTextViewportContains(const PageTextBounds& b, int x, int y) {
  return b.width > 0 && b.height > 0 && x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}

inline bool pageTextRangeContains(PageTextSourceView source, uint16_t first, uint16_t count, int x, int y) {
  if (!source.glyphs || first >= source.glyphCount || count > source.glyphCount - first) return false;
  for (uint16_t i = 0; i < count; ++i) {
    const auto& glyph = source.glyphs[first + i];
    if (glyph.pageWord != PageTextGlyph::kSyntheticPageWord &&
        pageTextViewportContains({glyph.x, glyph.y, glyph.width, glyph.height}, x, y)) return true;
  }
  return false;
}

inline PageTextBounds clipPageTextBounds(const PageTextBounds& bounds, const PageTextBounds& viewport) {
  const int x = std::max<int>(bounds.x, viewport.x), y = std::max<int>(bounds.y, viewport.y);
  const int right = std::min(bounds.x + bounds.width, viewport.x + viewport.width);
  const int bottom = std::min(bounds.y + bounds.height, viewport.y + viewport.height);
  if (right <= x || bottom <= y) return {};
  return {int16_t(x), int16_t(y), int16_t(right - x), int16_t(bottom - y)};
}

inline bool scrollPageTextToSelection(OwnedLookupTextSource& source, const PageTextBounds& viewport,
                                      uint16_t firstGlyph, uint16_t count) {
  PageTextBounds selected;
  if (viewport.width <= 0 || viewport.height <= 0 ||
      !unionPageTextGlyphBounds(source.view(), firstGlyph, count, selected))
    return false;
  int delta = 0;
  if (selected.y < viewport.y || selected.height > viewport.height)
    delta = viewport.y - selected.y;
  else if (selected.y + selected.height > viewport.y + viewport.height)
    delta = viewport.y + viewport.height - selected.y - selected.height;
  if (!delta) return false;
  for (uint16_t i = 0; i < source.glyphCount; ++i) {
    const auto& g = source.glyphs[i];
    if (g.height > 0 && (int(g.y) + delta < INT16_MIN || int(g.y) + delta + g.height > INT16_MAX)) return false;
  }
  for (uint16_t i = 0; i < source.glyphCount; ++i) {
    auto& g = source.glyphs[i];
    if (g.height > 0) g.y = int16_t(g.y + delta);
  }
  return true;
}
