#pragma once
#include <algorithm>
namespace manga {
// Main-thread intent, serialized with rendering by the reader's RenderLock.
// Always merge fresh edges before taking a move; boundary clamping belongs to
// navigation after coalescing, never to each side of an opposing input pair.
class PendingInput {
 public:
  void move(bool forward) { moves_ = std::clamp(moves_ + (forward ? 1 : -1), -64, 64); }
  int takeMove() {
    const int direction = (moves_ > 0) - (moves_ < 0);
    moves_ -= direction;
    return direction;
  }
  bool hasMove() const { return moves_ != 0; }
  void requestMenu() { menu_ = true; }
  bool takeMenu() {
    const bool requested = menu_;
    menu_ = false;
    return requested;
  }
  bool hasMenu() const { return menu_; }
  void rotate(bool clockwise) { rotations_ = (rotations_ + (clockwise ? 1 : 3)) % 4; }
  int takeRotations() {
    const int rotations = rotations_;
    rotations_ = 0;
    return rotations;
  }

 private:
  int moves_ = 0, rotations_ = 0;
  bool menu_ = false;
};
}  // namespace manga
