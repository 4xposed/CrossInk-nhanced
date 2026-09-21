#include <cassert>

#include "activities/home/LibraryNavigation.h"
int main() {
  library::Navigation nav;
  nav.count = 10;
  nav.capacity = 9;
  nav.columns = 3;
  nav.tabsFocused = false;
  nav.selected = 9;
  nav.clamp();
  assert(nav.pageStart() == 9);
  nav.count = 2;
  nav.clamp();
  assert(nav.selected == 1 && nav.pageStart() == 0);
  nav.selected = 0;
  nav.up();
  assert(nav.tabsFocused);
  nav.down();
  assert(!nav.tabsFocused && nav.selected == 0);
  nav.count = 0;
  nav.clamp();
  assert(nav.selected == 0 && nav.tabsFocused);
  for (auto size : {std::pair<int, int>{450, 620}, {740, 300}, {260, 180}}) {
    const auto grid = library::gridLayout(size.first, size.second, 28, 10);
    assert(grid.capacity() > 0 && grid.capacity() <= 9);
    assert(grid.columns * grid.cellWidth <= size.first);
    assert(grid.rows * grid.cellHeight <= size.second);
    assert(grid.coverHeight > 0 && grid.coverHeight + 56 <= grid.cellHeight);
  }
}
