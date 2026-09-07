#pragma once
#include <CooperativeCancellation.h>
#include <MangaCover.h>

#include <atomic>
#include <cstdint>

// Only generations cross tasks. Retry and active flags belong to the render owner.
class MangaCoverWork {
 public:
  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
  static_assert(alignof(std::atomic<uint32_t>) >= alignof(uint32_t));
  // Main is the ONLY writer. C3 has no RISC-V A extension: atomic RMW uses
  // IDF's interrupt-masked helper and is_always_lock_free is false. Aligned
  // load/store compile to lw/sw plus ordering fences on the pinned toolchain,
  // so this request never waits for the render owner (or calls an RMW helper).
  uint32_t requestCancellation() {
    const uint32_t next = generation.load(std::memory_order_relaxed) + 1;
    generation.store(next, std::memory_order_release);
    return next;
  }
  void authorizeIntent() { authorized.store(generation.load(std::memory_order_acquire), std::memory_order_release); }
  void authorizeIntent(uint32_t intent) {
    if (generation.load(std::memory_order_acquire) == intent) authorized.store(intent, std::memory_order_release);
  }
  struct Batch {
    MangaCoverWork& owner;
    const uint32_t generation;
    bool cancelled() const { return owner.generation.load(std::memory_order_acquire) != generation; }
    static bool poll(void* context) { return static_cast<Batch*>(context)->cancelled(); }
    CooperativeCancellation cancellation() { return {poll, this}; }
  };
  Batch batch() { return {*this, authorized.load(std::memory_order_acquire)}; }
  bool active = false;  // accessed only under RenderLock
 private:
  std::atomic<uint32_t> generation{1};
  std::atomic<uint32_t> authorized{0};
};

// Indexed alongside the owning recent-book path. Reset only for explicit reload
// or a new visit, so failed identities/sizes cannot cause repaint retry storms.
struct MangaCoverAttempt {
  bool attempted = false;
  uint32_t sourceCrc = 0;
  uint64_t sourceSize = 0;
  int width = 0, height = 0, secondWidth = 0, secondHeight = 0;
  void record(int w, int h, const manga::ThumbnailDiagnostics& diagnostics) {
    if (!width) {
      width = w;
      height = h;
    } else {
      secondWidth = w;
      secondHeight = h;
    }
    sourceCrc = diagnostics.sourceCrc;
    sourceSize = diagnostics.sourceSize;
  }
};
