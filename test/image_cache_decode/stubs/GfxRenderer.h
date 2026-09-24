#pragma once
#include <array>
#include <cstdint>
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_MSB, GRAYSCALE_LSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  static inline int accesses = 0;
  mutable std::array<uint8_t, 512 * 512 / 8> pixels{};
  Orientation orientation = LandscapeCounterClockwise;
  int getScreenWidth() const {
    ++accesses;
    return 512;
  }
  int getScreenHeight() const {
    ++accesses;
    return 512;
  }
  uint8_t* getWriteTarget() const {
    ++accesses;
    return pixels.data();
  }
  int getWriteOriginY() const {
    ++accesses;
    return 0;
  }
  int getWriteRows() const {
    ++accesses;
    return 512;
  }
  RenderMode getRenderMode() const {
    ++accesses;
    return BW;
  }
  int getDisplayWidthBytes() const {
    ++accesses;
    return 64;
  }
  int getDisplayWidth() const {
    ++accesses;
    return 512;
  }
  int getDisplayHeight() const {
    ++accesses;
    return 512;
  }
  Orientation getOrientation() const {
    ++accesses;
    return orientation;
  }
};
