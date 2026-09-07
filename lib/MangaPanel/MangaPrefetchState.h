#pragma once
#include <atomic>
#include <cstdint>

namespace manga {
// Single owner (serialized by RenderLock), single producer. Request/result bytes
// are non-atomic: release/acquire publishes them, and cannot overlap a new job.
class PrefetchState {
 public:
  bool idle() const { return state_.load(std::memory_order_acquire) == Idle; }
  bool post() {
    if (!idle()) return false;
    jobGeneration_ = generation_.load(std::memory_order_acquire);
    state_.store(Posted, std::memory_order_release);
    return true;
  }
  bool begin() {
    auto expected = Posted;
    return state_.compare_exchange_strong(expected, Running, std::memory_order_acq_rel);
  }
  void cancel() { generation_.fetch_add(1, std::memory_order_acq_rel); }
  bool cancelled() const { return generation_.load(std::memory_order_acquire) != jobGeneration_; }
  void finish() { state_.store(Finished, std::memory_order_release); }
  bool consume() {
    if (state_.load(std::memory_order_acquire) != Finished) return false;
    state_.store(Idle, std::memory_order_release);
    return true;
  }

 private:
  enum State : uint8_t { Idle, Posted, Running, Finished };
  std::atomic<State> state_{Idle};
  std::atomic<uint32_t> generation_{0};
  uint32_t jobGeneration_ = 0;
};
}  // namespace manga
