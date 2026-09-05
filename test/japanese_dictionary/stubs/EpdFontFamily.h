#pragma once

#include <cstdint>

namespace EpdFontFamily {
enum Style : uint8_t {
  REGULAR = 0,
  BOLD = 1,
  ITALIC = 2,
  BOLD_ITALIC = 3,
  UNDERLINE = 4,
  STRIKETHROUGH = 8,
  SUP = 16,
  SUB = 32,
};
}  // namespace EpdFontFamily
