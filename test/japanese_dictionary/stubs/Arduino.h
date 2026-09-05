#pragma once

#include <cstdint>

namespace dict_arduino_test {
inline uint32_t freeHeap = 96 * 1024;
inline uint32_t maxAllocHeap = 48 * 1024;

inline void reset() {
  freeHeap = 96 * 1024;
  maxAllocHeap = 48 * 1024;
}
}  // namespace dict_arduino_test

struct EspTestStub {
  uint32_t getFreeHeap() const { return dict_arduino_test::freeHeap; }
  uint32_t getMaxAllocHeap() const { return dict_arduino_test::maxAllocHeap; }
};

inline EspTestStub ESP;
