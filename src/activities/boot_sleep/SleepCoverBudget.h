#pragma once
#include <CooperativeCancellation.h>

#include <cstdint>
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <sdkconfig.h>
#endif

#ifndef CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS
#define CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS 2500
#endif
static_assert(CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS > 0);
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
static_assert(CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS <= CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 / 2);
#endif
// Unmeasured policy: half the configured five-second watchdog window. This is
// cooperative (slow SD/codec/close calls can overshoot), never a hard deadline.
class SleepCoverBudget {
 public:
  void begin(uint32_t (*clockFunction)(void*), void* context) {
    clock = clockFunction;
    clockContext = context;
    started = expired = false;
    largestGap = 0;
  }
  CooperativeCancellation cancellation() {
    if (!started && clock) {
      start = previous = clock(clockContext);
      started = true;
    }
    return {poll, this};
  }
  uint32_t elapsed() const { return started && clock ? uint32_t(clock(clockContext) - start) : 0; }
  uint32_t maximumPollGap() const { return largestGap; }
  bool cancelled() const { return expired; }

 private:
  static bool poll(void* context) {
    auto& self = *static_cast<SleepCoverBudget*>(context);
    if (self.expired) return true;
    if (!self.clock) return false;
    const uint32_t now = self.clock(self.clockContext);
    const uint32_t gap = now - self.previous;
    if (gap > self.largestGap) self.largestGap = gap;
    self.previous = now;
    self.expired = uint32_t(now - self.start) >= CROSSINK_SLEEP_COVER_GENERATION_BUDGET_MS;
    return self.expired;
  }
  uint32_t (*clock)(void*) = nullptr;
  void* clockContext = nullptr;
  uint32_t start = 0, previous = 0, largestGap = 0;
  bool started = false, expired = false;
};
