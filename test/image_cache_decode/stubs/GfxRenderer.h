#pragma once
#include <array>
#include <cstdint>
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_MSB, GRAYSCALE_LSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  static inline int accesses = 0;
  std::array<uint8_t, 512 * 512 / 8> pixels{};
  Orientation orientation = LandscapeCounterClockwise;
  int getScreenWidth() {
    ++accesses;
    return 512;
  }
  int getScreenHeight() {
    ++accesses;
    return 512;
  }
  uint8_t* getWriteTarget() {
    ++accesses;
    return pixels.data();
  }
  int getWriteOriginY() {
    ++accesses;
    return 0;
  }
  int getWriteRows() {
    ++accesses;
    return 512;
  }
  RenderMode getRenderMode() {
    ++accesses;
    return BW;
  }
  int getDisplayWidthBytes() {
    ++accesses;
    return 64;
  }
  int getDisplayWidth() {
    ++accesses;
    return 512;
  }
  int getDisplayHeight() {
    ++accesses;
    return 512;
  }
  Orientation getOrientation() {
    ++accesses;
    return orientation;
  }
};
