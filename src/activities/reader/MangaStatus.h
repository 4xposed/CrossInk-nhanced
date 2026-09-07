#pragma once
#include <algorithm>
class GfxRenderer;
namespace manga {
struct StatusRect {
  int x = 0, y = 0, width = 0, height = 0;
};
struct StatusLayout {
  StatusRect counter, hint;
};
inline StatusLayout layoutStatus(int x, int y, int width, int height, int counterWidth, int hintWidth, int lineHeight,
                                 bool panel) {
  if (width <= 0 || height <= 0) return {};
  const int h = std::min(height, std::max(0, lineHeight) + 4);
  const int cw = std::min(width, std::max(0, counterWidth) + 4);
  const int hw = panel ? std::min(std::max(0, width - cw - 2), std::max(0, hintWidth) + 4) : 0;
  return {{x + width - cw, y + height - h, cw, h}, {x, y + height - h, hw, h}};
}
// BW: opaque white patches + black glyphs. Both gray masks: zero rectangle,
// no glyphs. Reuse the same layout in every replay and restored-BW cleanup.
void drawStatus(const GfxRenderer& renderer, const StatusLayout& layout, int font, const char* counter,
                const char* hint, bool grayMask);
}  // namespace manga
