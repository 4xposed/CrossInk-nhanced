#pragma once
#include <cstdint>
namespace checked_directory {
enum class Probe : uint8_t { Reject, Current, LastEntry };
constexpr Probe plan(uint64_t before, uint64_t after, bool parentError) {
  if (parentError || (before & 31) || (after & 31) || after < before) return Probe::Reject;
  return after == before ? Probe::Current : Probe::LastEntry;
}
constexpr bool clean(Probe probe, int bytes, uint8_t firstByte, bool restored, bool parentError) {
  return restored && !parentError &&
         ((probe == Probe::Current && bytes == 0) || (probe == Probe::LastEntry && bytes == 32 && firstByte == 0));
}
}  // namespace checked_directory
