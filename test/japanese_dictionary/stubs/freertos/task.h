#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>

#include "FreeRTOS.h"

enum eNotifyAction { eNoAction = 0, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite };

namespace freertos_test {
struct TaskState {
  std::mutex mutex;
  std::condition_variable notification;
  TaskFunction_t function = nullptr;
  void* context = nullptr;
  uint32_t pendingNotifications = 0;
};

inline thread_local TaskState* currentTask = nullptr;
inline std::atomic_uint32_t delayCallCount = 0;
}  // namespace freertos_test

inline TaskHandle_t xTaskCreateStatic(TaskFunction_t task, const char*, uint32_t, void* context, UBaseType_t,
                                      StackType_t*, StaticTask_t* taskStorage) {
  if (task == nullptr || taskStorage == nullptr || taskStorage->testState != nullptr) return nullptr;

  // The detached host task owns this state for the test process lifetime,
  // mirroring the firmware worker task, which is created once and never exits.
  auto* state = new (std::nothrow) freertos_test::TaskState;
  if (state == nullptr) return nullptr;
  state->function = task;
  state->context = context;
  taskStorage->testState = state;
  std::thread([state]() {
    freertos_test::currentTask = state;
    state->function(state->context);
  }).detach();
  return taskStorage;
}

inline BaseType_t xTaskNotify(TaskHandle_t task, uint32_t, eNotifyAction action) {
  if (task == nullptr || task->testState == nullptr || action != eIncrement) return pdFALSE;
  auto* state = static_cast<freertos_test::TaskState*>(task->testState);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->pendingNotifications;
  }
  state->notification.notify_one();
  return pdTRUE;
}

inline uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t) {
  auto* state = freertos_test::currentTask;
  if (state == nullptr) return 0;
  std::unique_lock<std::mutex> lock(state->mutex);
  state->notification.wait(lock, [state]() { return state->pendingNotifications != 0; });
  const uint32_t pending = state->pendingNotifications;
  if (clearOnExit == pdTRUE)
    state->pendingNotifications = 0;
  else
    --state->pendingNotifications;
  return pending;
}

inline void vTaskDelay(TickType_t ticks) {
  ++freertos_test::delayCallCount;
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks * portTICK_PERIOD_MS));
}

inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
