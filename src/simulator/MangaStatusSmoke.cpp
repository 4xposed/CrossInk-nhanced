#ifdef SIMULATOR
#include "MangaStatusSmoke.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include "activities/reader/MangaStatus.h"
#include "fontIds.h"
namespace {
bool pixel(const GfxRenderer& r, int x, int y) {
  const int w = r.getDisplayWidth(), h = r.getDisplayHeight();
  int px = x, py = y;
  switch (r.getOrientation()) {
    case GfxRenderer::Portrait:
      px = y;
      py = h - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      px = w - 1 - x;
      py = h - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      px = w - 1 - y;
      py = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      break;
  }
  const size_t bit = static_cast<size_t>(py) * w + px;
  return (r.getFrameBuffer()[bit / 8] & (0x80 >> (bit % 8))) != 0;
}
bool patch(const GfxRenderer& r, const manga::StatusRect& p, bool mask) {
  int black = 0, white = 0;
  for (int y = p.y; y < p.y + p.height; ++y)
    for (int x = p.x; x < p.x + p.width; ++x) {
      const bool value = pixel(r, x, y);
      if (mask && value) return false;
      black += !value;
      white += value;
      if (!mask && (x == p.x || y == p.y || x == p.x + p.width - 1 || y == p.y + p.height - 1) && !value) return false;
    }
  return mask || (black > 0 && white > 0);
}
uint32_t hash(const GfxRenderer& r) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < r.getBufferSize(); ++i) h = (h ^ r.getFrameBuffer()[i]) * 16777619u;
  return h;
}
}  // namespace
bool verifyMangaStatusPlanes(GfxRenderer& r) {
  const auto orientation = r.getOrientation();
  ScopedCleanup restore{[&] {
    r.setOrientation(orientation);
    r.setRenderMode(GfxRenderer::BW);
  }};
  for (int o = 0; o < 4; ++o) {
    r.setOrientation(static_cast<GfxRenderer::Orientation>(o));
    int t, rr, b, l;
    r.getOrientedViewableTRBL(&t, &rr, &b, &l);
    // Additional asymmetric insets exercise geometry independently of board defaults.
    const auto layout =
        manga::layoutStatus(l + 3, t + 7, r.getScreenWidth() - l - rr - 14, r.getScreenHeight() - t - b - 20,
                            r.getTextWidth(UI_10_FONT_ID, "2/4  7/19"), r.getTextWidth(UI_10_FONT_ID, "Panels"),
                            r.getLineHeight(UI_10_FONT_ID), true);
    r.setRenderMode(GfxRenderer::BW);
    r.clearScreen(0x55);
    manga::drawStatus(r, layout, UI_10_FONT_ID, "2/4  7/19", "Panels", false);
    if (!patch(r, layout.counter, false) || !patch(r, layout.hint, false)) return false;
    const uint32_t bw = hash(r);
    r.displayBuffer();
    r.preconditionGrayscale();
    r.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    r.clearScreen(0xff);
    manga::drawStatus(r, layout, UI_10_FONT_ID, "2/4  7/19", "Panels", true);
    if (!patch(r, layout.counter, true) || !patch(r, layout.hint, true)) return false;
    r.copyGrayscaleLsbBuffers();
    r.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    r.clearScreen(0xff);
    manga::drawStatus(r, layout, UI_10_FONT_ID, "2/4  7/19", "Panels", true);
    if (!patch(r, layout.counter, true) || !patch(r, layout.hint, true)) return false;
    r.copyGrayscaleMsbBuffers();
    r.displayGrayBuffer();
    r.setRenderMode(GfxRenderer::BW);
    r.clearScreen(0x55);
    manga::drawStatus(r, layout, UI_10_FONT_ID, "2/4  7/19", "Panels", false);
    if (hash(r) != bw) return false;
    r.cleanupGrayscaleWithFrameBuffer();
    if (hash(r) != bw) return false;
  }
  LOG_INF("SMOKE", "Verified actual manga status BW/LSB/MSB/restored cleanup pixels in four orientations");
  return true;
}
#endif
