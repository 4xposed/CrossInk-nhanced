#include <cassert>
#include <cstdio>

#include "MangaMenuState.h"
#include "MangaNavigation.h"

int main() {
  using namespace manga;
  const MenuAction expected[] = {MenuAction::Chapter,        MenuAction::Percent,        MenuAction::Bookmarks,
                                 MenuAction::ToggleBookmark, MenuAction::PanelsOnly,     MenuAction::PanelRotation,
                                 MenuAction::Orientation,    MenuAction::Home,           MenuAction::Lookup,
                                 MenuAction::LookupHistory,  MenuAction::ReaderSettings, MenuAction::AutoTurn,
                                 MenuAction::Screenshot,     MenuAction::DeleteCache,    MenuAction::OcrQr};
  for (int i = 0; i < 15; ++i) assert(menuActionAt(i) == expected[i]);
  assert(menuActionAt(-1) == MenuAction::None && menuActionAt(15) == MenuAction::None);
  constexpr uint32_t intervals[] = {0, 60000, 20000, 10000, 5000};
  for (int rate = 0; rate < 5; ++rate) {
    AutoTurn clock;
    clock.select(rate, 100);
    assert(clock.active() == (rate != 0));
    assert(clock.intervalMs() == intervals[rate]);
    if (!rate) {
      assert(!clock.poll(100000, true));
      continue;
    }
    assert(!clock.poll(99 + intervals[rate], true));
    assert(clock.poll(100 + intervals[rate], true));
    assert(!clock.poll(100 + intervals[rate], true));
    clock.rendered(150 + intervals[rate]);
    assert(!clock.poll(149 + 2 * intervals[rate], true));
    assert(clock.poll(150 + 2 * intervals[rate], true));
    clock.cancel();
    assert(!clock.active() && !clock.poll(999999, true));
  }
  AutoTurn busy;
  busy.select(4, 0);
  assert(!busy.poll(6000, false));
  assert(!busy.poll(100000, false));
  assert(!busy.poll(100100, true));
  assert(!busy.poll(105099, true));
  assert(busy.poll(105100, true));
  AutoTurn wrapped;
  wrapped.select(4, 0xfffffff0U);
  assert(!wrapped.poll(4983, true));
  assert(wrapped.poll(4984, true));
  AutoTurn slowRender;
  slowRender.select(4, 0);
  slowRender.rendered(6000);
  assert(!slowRender.poll(6000, true));
  assert(!slowRender.poll(10999, true));
  assert(slowRender.poll(11000, true));
  AutoTurn invalid;
  invalid.select(99, 0);
  assert(!invalid.active());
  PageAvailability p;
  p.overview = true;
  p.panelCount = 4;
  p.setCrop(0);
  p.setCrop(3);
  auto automatic = automaticNext({0, -1}, 3, p, false);
  assert(automatic.changed && automatic.changePage && automatic.position.page == 1 &&
         automatic.entry == Entry::Overview);
  auto manual = next({0, -1}, 3, p, false);
  assert(manual.position.panel == 0 && !manual.changePage);
  automatic = automaticNext({0, 0}, 3, p, false);
  assert(automatic.position.panel == 3 && !automatic.changePage);
  automatic = automaticNext({0, 3}, 3, p, false);
  assert(automatic.changePage && automatic.position.page == 1);
  assert(!automaticNext({2, -1}, 3, p, false).changed);
  assert(!automaticNext({0, -1}, 0, p, false).changed);
  auto dest = resolveEntry(1, Entry::Overview, p, true);
  assert(dest.panel == 0);
  p.overview = false;
  dest = resolveEntry(1, Entry::Overview, p, false);
  assert(dest.panel == 0);
  p = {};
  dest = resolveEntry(1, Entry::Overview, p, true);
  assert(dest.panel == -1);
  puts("Manga menu/navigation/clock contracts passed");
}
