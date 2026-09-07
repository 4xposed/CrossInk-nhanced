#pragma once
#include <cstdint>
namespace HalDisplay {
enum class RefreshMode { HALF_REFRESH };
}
class GfxRenderer {
 public:
  const uint8_t* getFrameBuffer() const { return nullptr; }
  int getDisplayWidth() const { return 8; }
  int getDisplayHeight() const { return 8; }
  int getScreenWidth() const { return 8; }
  int getScreenHeight() const { return 8; }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) const { *t = *r = *b = *l = 0; }
  void invertRect(int, int, int, int) const {}
  void displayBuffer() const {}
  void displayBuffer(HalDisplay::RefreshMode) const {}
};
