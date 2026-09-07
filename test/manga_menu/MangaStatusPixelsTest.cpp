#include <GfxRenderer.h>

#include <cassert>
#include <cstdio>

#include "MangaStatus.h"
int main() {
  GfxRenderer renderer;
  const auto layout = manga::layoutStatus(7, 11, 220, 290, 40, 25, 12, true);
  const auto check = [&](bool gray) {
    for (const auto& r : {layout.counter, layout.hint})
      for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x) {
          const bool glyph = x >= r.x + 2 && x < r.x + 5 && y >= r.y + 2 && y < r.y + 7;
          assert(renderer.pixel(x, y) == (!gray && !glyph));
        }
  };
  renderer.bits.fill(0x55);
  manga::drawStatus(renderer, layout, 1, "12/99", "Panels", false);
  check(false);
  const auto bw = renderer.bits;
  // Seed selected gray transitions under both patches, including every edge.
  renderer.bits.fill(0xff);
  manga::drawStatus(renderer, layout, 1, "12/99", "Panels", true);
  check(true);
  assert(renderer.pixel(0, 0));
  renderer.bits.fill(0xff);
  manga::drawStatus(renderer, layout, 1, "12/99", "Panels", true);
  check(true);
  renderer.bits.fill(0x55);
  manga::drawStatus(renderer, layout, 1, "12/99", "Panels", false);
  check(false);
  assert(renderer.bits == bw);
  puts("Status BW/glyph/LSB/MSB/restored patch bits passed");
}
