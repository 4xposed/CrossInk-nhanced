#pragma once

#include <cstdint>

using BaseType_t = int32_t;
using UBaseType_t = uint32_t;
using TickType_t = uint32_t;
using StackType_t = uint32_t;

struct StaticTask_t {
  void* testState = nullptr;
};

using TaskHandle_t = StaticTask_t*;
using TaskFunction_t = void (*)(void*);

constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t portTICK_PERIOD_MS = 1;
