#pragma once
#include <algorithm>
#include <cstdint>
namespace library {
struct GridLayout {
  int columns, rows, cellWidth, cellHeight, coverWidth, coverHeight;
  int capacity() const { return columns * rows; }
};
inline GridLayout gridLayout(int width, int height, int titleLineHeight, int gap) {
  const int columns = std::clamp(width / 140, 1, 3);
  const int rows = std::clamp(height / (140 + titleLineHeight * 2 + gap), 1, 3);
  const int cellWidth = std::max(1, width / columns);
  const int cellHeight = std::max(1, height / rows);
  const int coverHeight =
      std::max(1, std::min(cellHeight - titleLineHeight * 2 - gap * 2, (cellWidth - gap * 2) * 5 / 3));
  return {columns, rows, cellWidth, cellHeight, std::max(1, coverHeight * 3 / 5), coverHeight};
}
struct Navigation {
  uint32_t count = 0, selected = 0;
  int capacity = 1, columns = 1;
  bool tabsFocused = true;
  uint32_t pageStart() const { return selected / capacity * capacity; }
  void clamp() {
    selected = count ? std::min(selected, count - 1) : 0;
    if (!count) tabsFocused = true;
  }
  void move(int delta) {
    if (!count) return;
    const int64_t next = int64_t(selected) + delta;
    selected = static_cast<uint32_t>(std::clamp<int64_t>(next, 0, count - 1));
  }
  void up() {
    if (selected < static_cast<uint32_t>(columns))
      tabsFocused = true;
    else
      move(-columns);
  }
  void down() {
    if (tabsFocused) {
      if (count) tabsFocused = false;
    } else
      move(columns);
  }
};
}  // namespace library
