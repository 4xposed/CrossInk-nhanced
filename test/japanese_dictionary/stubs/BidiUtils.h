#pragma once

#include <cstdint>

namespace BidiUtils {
inline uint8_t detectParagraphLevel(const char*, const uint8_t fallbackLevel = 0) { return fallbackLevel; }
}  // namespace BidiUtils
