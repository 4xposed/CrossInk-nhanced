#pragma once
#include <cstdint>

#include "Print.h"
inline void vTaskDelay(int) {}
struct TestEsp {
  unsigned getFreeHeap() { return 1000000; }
};
inline TestEsp ESP;
