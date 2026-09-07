#pragma once
#include <Print.h>

// Borrow the destination; any short write remains a failure for the entire stream.
class CheckedPrint final : public Print {
 public:
  explicit CheckedPrint(Print& destination) : destination(destination) {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (failed) return 0;
    const size_t written = destination.write(bytes, size);
    failed = written != size;
    return written;
  }
  bool good() const { return !failed; }

 private:
  Print& destination;
  bool failed = false;
};
