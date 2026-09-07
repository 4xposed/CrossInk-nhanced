#include <cassert>
#include <cstdio>

#include "MangaStatus.h"
int main() {
  for (int orientation = 0; orientation < 4; ++orientation) {
    const int w = orientation % 2 ? 800 : 480, h = orientation % 2 ? 480 : 800;
    auto layout = manga::layoutStatus(13, 17, w - 42, h - 48, 120, 90, 18, true);
    assert(layout.counter.x + layout.counter.width <= w - 29);
    assert(layout.counter.y + layout.counter.height <= h - 31);
    assert(layout.hint.x >= 13 && layout.hint.width == 94);
    assert(layout.hint.x + layout.hint.width < layout.counter.x);
    auto longHint = manga::layoutStatus(13, 17, w - 42, h - 48, 120, 10000, 18, true);
    assert(longHint.hint.x + longHint.hint.width <= longHint.counter.x - 2);
    auto overview = manga::layoutStatus(13, 17, w - 42, h - 48, 120, 90, 18, false);
    assert(overview.hint.width == 0);
  }
  auto tiny = manga::layoutStatus(0, 0, 2, 2, 300, 100, 18, true);
  assert(tiny.counter.width <= 2 && tiny.counter.height <= 2 && tiny.hint.width == 0);
  puts("Manga status safe-viewport layout passed");
}
