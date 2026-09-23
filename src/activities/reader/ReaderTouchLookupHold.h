#pragma once

// Keep a recognized word hold until the render task releases the displayed page.
// Coordinates belong to the original contact, even if the finger moves or lifts.
class ReaderTouchLookupHold {
 public:
  bool capture(bool candidate, int x, int y, unsigned long heldMs) {
    if (!candidate) {
      handled_ = false;
      return false;
    }
    if (pending_ || handled_ || heldMs < 500UL) return false;
    x_ = x;
    y_ = y;
    handled_ = true;
    pending_ = true;
    return true;
  }
  bool pending() const { return pending_; }
  int x() const { return x_; }
  int y() const { return y_; }
  void consume() { pending_ = false; }
  void reset() { *this = {}; }

 private:
  int x_ = 0;
  int y_ = 0;
  bool handled_ = false;
  bool pending_ = false;
};
