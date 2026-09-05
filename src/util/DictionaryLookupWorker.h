#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

struct DictionaryWorkerJob {
  using RunCallback = void (*)(void*);

  void* owner = nullptr;
  RunCallback run = nullptr;
};

// A single, static dictionary worker avoids allocating a 4 KB task stack for
// every lookup. Job owners still own all lookup state and must cancel and wait
// for the worker before they are destroyed.
class DictionaryLookupWorker {
 public:
  static DictionaryLookupWorker& instance();

  // Starts a job. Returns false when the job is invalid, the static task could
  // not be created, or another owner still owns the worker.
  bool start(DictionaryWorkerJob job);
  bool isBusy() const;
  bool owns(const void* owner) const;

  // Waits until owner is no longer running on the worker. The owner must set
  // its cancellation flag before calling this method.
  void waitForOwner(const void* owner);

 private:
  static constexpr size_t kStackBytes = 4096;
  // ESP-IDF's xTaskCreateStatic takes its depth in bytes. Keep the storage
  // expressed in StackType_t units so the backing buffer is sized correctly.
  static constexpr size_t kStackWords = (kStackBytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);

  static void taskEntry(void* context);
  void run();

  StaticTask_t taskStorage_ = {};
  StackType_t stack_[kStackWords] = {};
  TaskHandle_t taskHandle_ = nullptr;
  std::atomic<void*> owner_{nullptr};
  std::atomic<DictionaryWorkerJob::RunCallback> run_{nullptr};
};
