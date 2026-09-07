#include "MangaStatus.h"

#include <GfxRenderer.h>
namespace manga {
void drawStatus(const GfxRenderer& renderer, const StatusLayout& layout, const int font, const char* counter,
                const char* hint, const bool grayMask) {
  const auto patch = [&](const StatusRect& r, const char* text) {
    if (r.width <= 0 || r.height <= 0) return;
    // fillRect's solid black encoding is zero in both BW and mask buffers.
    renderer.fillRect(r.x, r.y, r.width, r.height, grayMask);
    if (!grayMask && text) {
      renderer.beginTextClip(r.x, r.y, r.width, r.height);
      renderer.drawText(font, r.x + 2, r.y + 2, text, true);
      renderer.endTextClip();
    }
  };
  patch(layout.counter, counter);
  patch(layout.hint, hint);
}
}  // namespace manga
