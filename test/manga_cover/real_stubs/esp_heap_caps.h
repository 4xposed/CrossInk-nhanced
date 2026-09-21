#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "FaultAllocation.h"

constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_INTERNAL = 2;
constexpr uint32_t MALLOC_CAP_SPIRAM = 4;
constexpr uint32_t MALLOC_CAP_8BIT = 8;

// Model the C3 heap while routing every owned byte buffer through fault injection.
inline size_t heap_caps_get_total_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 0 : 1000000; }
inline size_t heap_caps_get_free_size(uint32_t caps) { return heap_caps_get_total_size(caps); }
inline size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_total_size(caps); }
inline void* heap_caps_malloc(size_t bytes, uint32_t caps) {
  return caps & MALLOC_CAP_SPIRAM ? nullptr : coverTestMalloc(bytes);
}
inline void* heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps) {
  if (alignment > alignof(std::max_align_t)) return nullptr;
  return heap_caps_malloc(bytes, caps);
}
inline void heap_caps_free(void* ptr) { std::free(ptr); }
