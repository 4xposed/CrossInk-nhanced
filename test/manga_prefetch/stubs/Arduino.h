#pragma once
#include <freertos/task.h>

#include <cstdint>
inline uint32_t millis() { return 0; }
struct TestEsp {
  uint32_t getFreeHeap() { return 1000000; }
  uint32_t getMaxAllocHeap() { return 1000000; }
};
inline TestEsp ESP;
