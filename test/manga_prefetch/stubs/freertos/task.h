#pragma once
#include <atomic>
#include <chrono>
#include <thread>
using TaskHandle_t = void*;
namespace task_test {
inline bool failCreate = false;
inline std::thread thread;
inline std::atomic<bool> pause{false};
}  // namespace task_test
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, unsigned, void* context, int, TaskHandle_t* handle,
                                   int) {
  if (task_test::failCreate) return 0;
  *handle = context;
  task_test::thread = std::thread([=] {
    while (task_test::pause.load()) std::this_thread::yield();
    fn(context);
  });
  return 1;
}
inline void vTaskDelay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void vTaskDelete(void*) {}
