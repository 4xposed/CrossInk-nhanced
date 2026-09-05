#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace dict_memory_test {
inline size_t rejectedBytes = 0;
inline size_t largestRequest = 0;
inline size_t totalRequested = 0;
inline size_t requestCount = 0;
inline size_t rejectedRequest = 0;
inline size_t rejectThroughRequest = 0;
inline bool rejectAll = false;
inline std::array<size_t, 32> requests{};

inline void reset() {
  rejectedBytes = 0;
  largestRequest = 0;
  totalRequested = 0;
  requestCount = 0;
  rejectedRequest = 0;
  rejectThroughRequest = 0;
  rejectAll = false;
  requests.fill(0);
}

inline bool reject(size_t bytes) {
  ++requestCount;
  totalRequested += bytes;
  if (requestCount <= requests.size()) requests[requestCount - 1] = bytes;
  if (bytes > largestRequest) largestRequest = bytes;
  return rejectAll || requestCount <= rejectThroughRequest || (rejectedBytes != 0 && rejectedBytes == bytes) ||
         (rejectedRequest != 0 && rejectedRequest == requestCount);
}
}  // namespace dict_memory_test

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (dict_memory_test::reject(sizeof(T))) return nullptr;
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Element = std::remove_extent_t<T>;
  if (count > SIZE_MAX / sizeof(Element)) return nullptr;
  if (dict_memory_test::reject(count * sizeof(Element))) return nullptr;
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
