#include "DictionaryLookupWorker.h"

#include <Logging.h>

DictionaryLookupWorker& DictionaryLookupWorker::instance() {
  static DictionaryLookupWorker worker;
  return worker;
}

bool DictionaryLookupWorker::start(const DictionaryWorkerJob job) {
  if (job.owner == nullptr || job.run == nullptr) {
    LOG_ERR("DICT", "Could not start invalid dictionary worker job");
    return false;
  }

  if (taskHandle_ == nullptr) {
    taskHandle_ = xTaskCreateStatic(taskEntry, "DictLookup", kStackBytes, this, 1, stack_, &taskStorage_);
    if (taskHandle_ == nullptr) {
      LOG_ERR("DICT", "Could not start static dictionary lookup worker");
      return false;
    }
  }

  void* expected = nullptr;
  if (!owner_.compare_exchange_strong(expected, job.owner, std::memory_order_acq_rel, std::memory_order_acquire)) {
    LOG_ERR("DICT", "Dictionary lookup worker is busy");
    return false;
  }

  run_.store(job.run, std::memory_order_release);
  xTaskNotify(taskHandle_, 1, eIncrement);
  return true;
}

bool DictionaryLookupWorker::isBusy() const { return owner_.load(std::memory_order_acquire) != nullptr; }

bool DictionaryLookupWorker::owns(const void* owner) const {
  return owner != nullptr && owner_.load(std::memory_order_acquire) == owner;
}

void DictionaryLookupWorker::waitForOwner(const void* owner) {
  while (owns(owner)) vTaskDelay(1);
}

void DictionaryLookupWorker::taskEntry(void* context) { static_cast<DictionaryLookupWorker*>(context)->run(); }

void DictionaryLookupWorker::run() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    void* owner = owner_.load(std::memory_order_acquire);
    if (owner == nullptr) continue;
    const DictionaryWorkerJob::RunCallback callback = run_.load(std::memory_order_acquire);
    if (callback == nullptr) {
      LOG_ERR("DICT", "Dictionary worker woke without a callback");
      owner_.store(nullptr, std::memory_order_release);
      continue;
    }
    callback(owner);
    run_.store(nullptr, std::memory_order_relaxed);
    owner_.store(nullptr, std::memory_order_release);
  }
}
