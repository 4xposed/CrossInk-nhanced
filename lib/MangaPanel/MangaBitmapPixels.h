#pragma once

#include <CooperativeCancellation.h>

#include <cstddef>
#include <cstdint>

namespace manga {

// Worst-case working set at the supported 2048-pixel source width:
// 8192-byte 32-bpp BMP row + 512-byte decoded 2-bit row + 512-byte output row.
constexpr size_t kBitmapPixelScratchBytes = 9216;

struct BitmapPixelInfo {
  int width = 0;
  int height = 0;
  uint16_t bitsPerPixel = 0;
  bool topDown = false;
};

// Strict, bounded probe for the BITMAPINFOHEADER/BI_RGB subset consumed below.
// Reads no pixel data and performs no image-sized allocation.
bool probeBitmapPixels(const char* source, BitmapPixelInfo& info, CooperativeCancellation cancellation = {});

// Converts a BMP into the decoder pixel-cache body format. The output is a raw
// uint16_t little-endian width/height header followed by logical top-down,
// row-major 2-bit pixels (four per byte, most-significant pixel first).
// temporaryPath is removed on every failure.
bool writeBitmapPixels(const char* source, const char* temporaryPath, int width, int height, uint8_t* scratch,
                       size_t capacity, CooperativeCancellation cancellation = {});

}  // namespace manga
