#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
class GfxRenderer {
 public:
  static constexpr int width = 240, height = 320;
  mutable std::array<uint8_t, width * height / 8> bits{};
  mutable int calls = 0;
  int getScreenWidth() const { return width; }
  int getScreenHeight() const { return height; }
  void fillRect(int x, int y, int w, int h, bool black = true) const {
    ++calls;
    for (int yy = std::max(0, y); yy < std::min(height, y + h); ++yy)
      for (int xx = std::max(0, x); xx < std::min(width, x + w); ++xx) set(xx, yy, !black);
  }
  void beginTextClip(int x, int y, int w, int h) const {
    l = x;
    t = y;
    r = x + w;
    b = y + h;
  }
  void endTextClip() const {
    l = 0;
    t = 0;
    r = width;
    b = height;
  }
  void drawText(int, int x, int y, const char*, bool black = true) const {
    for (int yy = y; yy < y + 5; ++yy)
      for (int xx = x; xx < x + 3; ++xx)
        if (xx >= l && xx < r && yy >= t && yy < b) set(xx, yy, !black);
  }
  bool pixel(int x, int y) const { return bits[(y * width + x) / 8] & (0x80 >> ((y * width + x) % 8)); }

 private:
  mutable int l = 0, t = 0, r = width, b = height;
  void set(int x, int y, bool white) const {
    const int i = y * width + x;
    const uint8_t m = 0x80 >> (i % 8);
    if (white)
      bits[i / 8] |= m;
    else
      bits[i / 8] &= ~m;
  }
};
