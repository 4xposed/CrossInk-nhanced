#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace QrUtils {
constexpr size_t kMaxPayloadBytes = 2953;
// Conservative ECC_LOW byte capacities, including mode/count bits. The pinned
// codec does NOT reject overflow; choose a sufficient version before encoding.
constexpr uint8_t versionForBytes(size_t length) {
  return !length                      ? 0
         : length <= 78               ? 4
         : length <= 271              ? 10
         : length <= 858              ? 20
         : length <= 1732             ? 30
         : length <= kMaxPayloadBytes ? 40
                                      : 0;
}
constexpr size_t gridBytesForPayload(size_t length) {
  const auto version = versionForBytes(length);
  const size_t side = 17 + 4 * version;
  return version ? (side * side + 7) / 8 : 0;
}
struct OwnedPayload {
  std::unique_ptr<char[]> bytes;
  uint16_t length = 0;
  bool truncated = false;
};
}  // namespace QrUtils
