#pragma once
#include <cstdint>
inline uint32_t millis() { return 0; }
inline void vTaskDelay(int) {}
struct TestEsp {
  uint32_t getFreeHeap() { return 1000000; }
  uint32_t getMaxAllocHeap() { return 1000000; }
};
inline TestEsp ESP;
