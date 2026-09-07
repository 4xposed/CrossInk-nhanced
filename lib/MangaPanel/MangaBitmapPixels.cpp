#include "MangaBitmapPixels.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace manga {
namespace {
constexpr int kMaxSourceWidth = 2048;
constexpr int kMaxSourceHeight = 3072;

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readU32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool closeFile(FsFile& file, const char* label) {
  (void)label;
  if (!file.isOpen()) return true;
  if (file.close()) return true;
  LOG_ERR("MBP", "Failed to close %s", label);
  return false;
}

void removePartial(const char* path) {
  if (path && path[0] != '\0') Storage.remove(path);
}

bool writeExact(FsFile& file, const void* data, size_t size, const char* what) {
  (void)what;
  if (file.write(data, size) == size) return true;
  LOG_ERR("MBP", "Failed to write %s", what);
  return false;
}
}  // namespace

bool probeBitmapPixels(const char* source, BitmapPixelInfo& info, CooperativeCancellation cancellation) {
  info = {};
  if (cancellation.requested() || !source || source[0] == '\0') return false;

  FsFile file;
  if (!Storage.openFileForRead("MBP", source, file)) return false;
  uint8_t header[54];
  const uint64_t fileBytes = file.fileSize64();
  bool valid = fileBytes >= sizeof(header) && file.read(header, sizeof(header)) == static_cast<int>(sizeof(header));
  if (valid) {
    const uint32_t declaredBytes = readU32(header + 2);
    const uint32_t pixelOffset = readU32(header + 10);
    const uint32_t dibBytes = readU32(header + 14);
    const int32_t signedWidth = static_cast<int32_t>(readU32(header + 18));
    const int32_t signedHeight = static_cast<int32_t>(readU32(header + 22));
    const uint16_t planes = readU16(header + 26);
    const uint16_t bpp = readU16(header + 28);
    const uint32_t compression = readU32(header + 30);
    uint32_t colors = readU32(header + 46);
    const bool supportedBpp = bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32;
    if (colors == 0 && bpp <= 8) colors = 1U << bpp;
    const uint64_t paletteEnd = sizeof(header) + static_cast<uint64_t>(colors) * 4U;
    const uint64_t rowBytes =
        supportedBpp && signedWidth > 0 ? (static_cast<uint64_t>(signedWidth) * bpp + 31U) / 32U * 4U : 0;
    const uint64_t absHeight = signedHeight < 0 ? -static_cast<int64_t>(signedHeight) : signedHeight;
    const uint64_t pixelEnd = static_cast<uint64_t>(pixelOffset) + rowBytes * absHeight;
    valid = readU16(header) == 0x4d42 && dibBytes == 40 && signedWidth > 0 && absHeight > 0 &&
            signedWidth <= kMaxSourceWidth && absHeight <= kMaxSourceHeight && planes == 1 && supportedBpp &&
            compression == 0 && colors <= 256 && pixelOffset >= paletteEnd && pixelEnd >= pixelOffset &&
            pixelEnd <= fileBytes && declaredBytes >= pixelEnd && declaredBytes <= fileBytes;
    if (valid) {
      info.width = signedWidth;
      info.height = static_cast<int>(absHeight);
      info.bitsPerPixel = bpp;
      info.topDown = signedHeight < 0;
    }
  }
  if (!closeFile(file, "BMP probe")) valid = false;
  if (!valid) LOG_ERR("MBP", "Rejected malformed or unsupported BMP: %s", source);
  return valid && !cancellation.requested();
}

bool writeBitmapPixels(const char* source, const char* temporaryPath, const int width, const int height,
                       uint8_t* scratch, const size_t capacity, CooperativeCancellation cancellation) {
  if (cancellation.requested()) {
    removePartial(temporaryPath);
    return false;
  }
  if (!source || source[0] == '\0' || !temporaryPath || temporaryPath[0] == '\0' || !scratch || width <= 0 ||
      height <= 0 || width > kMaxSourceWidth || height > kMaxSourceHeight) {
    LOG_ERR("MBP", "Invalid bitmap pixel writer arguments");
    removePartial(temporaryPath);
    return false;
  }

  BitmapPixelInfo info;
  if (!probeBitmapPixels(source, info, cancellation) || width > info.width || height > info.height) {
    LOG_ERR("MBP", "Unsupported BMP or target dimensions: target=%dx%d", width, height);
    removePartial(temporaryPath);
    return false;
  }

  FsFile input;
  if (!Storage.openFileForRead("MBP", source, input)) {
    LOG_ERR("MBP", "Failed to open BMP source: %s", source);
    removePartial(temporaryPath);
    return false;
  }

  Bitmap bitmap(input, false);
  const BmpReaderError parseResult = bitmap.parseHeaders();
  const int sourceWidth = bitmap.getWidth();
  const int sourceHeight = bitmap.getHeight();
  if (parseResult != BmpReaderError::Ok || sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > kMaxSourceWidth ||
      sourceHeight > kMaxSourceHeight || sourceWidth != info.width || sourceHeight != info.height) {
    LOG_ERR("MBP", "Unsupported BMP or target dimensions: source=%dx%d target=%dx%d error=%s", sourceWidth,
            sourceHeight, width, height, Bitmap::errorToString(parseResult));
    closeFile(input, "BMP source");
    removePartial(temporaryPath);
    return false;
  }

  const size_t rawRowBytes = static_cast<size_t>(bitmap.getRowBytes());
  const size_t decodedRowBytes = (static_cast<size_t>(sourceWidth) + 3U) / 4U;
  const size_t outputRowBytes = (static_cast<size_t>(width) + 3U) / 4U;
  if (rawRowBytes > kBitmapPixelScratchBytes || decodedRowBytes > kBitmapPixelScratchBytes - rawRowBytes ||
      outputRowBytes > kBitmapPixelScratchBytes - rawRowBytes - decodedRowBytes ||
      capacity < rawRowBytes + decodedRowBytes + outputRowBytes) {
    LOG_ERR("MBP", "Bitmap pixel scratch too small: need=%u have=%u",
            unsigned(rawRowBytes + decodedRowBytes + outputRowBytes), unsigned(capacity));
    closeFile(input, "BMP source");
    removePartial(temporaryPath);
    return false;
  }

  uint8_t* rawRow = scratch;
  uint8_t* decodedRow = rawRow + rawRowBytes;
  uint8_t* outputRow = decodedRow + decodedRowBytes;

  FsFile output;
  if (!Storage.openFileForWrite("MBP", temporaryPath, output)) {
    LOG_ERR("MBP", "Failed to open temporary pixel output: %s", temporaryPath);
    closeFile(input, "BMP source");
    removePartial(temporaryPath);
    return false;
  }

  const uint8_t header[4] = {static_cast<uint8_t>(width), static_cast<uint8_t>(width >> 8),
                             static_cast<uint8_t>(height), static_cast<uint8_t>(height >> 8)};
  bool ok = writeExact(output, header, sizeof(header), "pixel header");

  for (int physicalRow = 0; ok && physicalRow < sourceHeight; ++physicalRow) {
    if (cancellation.requested()) {
      ok = false;
      break;
    }
    const BmpReaderError readResult = bitmap.readNextRow(decodedRow, rawRow);
    if (readResult != BmpReaderError::Ok) {
      LOG_ERR("MBP", "Failed reading BMP row %d: %s", physicalRow, Bitmap::errorToString(readResult));
      ok = false;
      break;
    }

    const int sourceY = bitmap.isTopDown() ? physicalRow : sourceHeight - 1 - physicalRow;
    const int outputY = static_cast<int>((static_cast<int64_t>(sourceY) * height + sourceHeight - 1) / sourceHeight);
    if (outputY >= height || static_cast<int64_t>(outputY) * sourceHeight / height != sourceY) continue;

    memset(outputRow, 0, outputRowBytes);
    for (int outputX = 0; outputX < width; ++outputX) {
      const int sourceX = static_cast<int>(static_cast<int64_t>(outputX) * sourceWidth / width);
      const uint8_t level = (decodedRow[sourceX >> 2] >> (6 - ((sourceX & 3) * 2))) & 0x03;
      outputRow[outputX >> 2] |= static_cast<uint8_t>(level << (6 - ((outputX & 3) * 2)));
    }

    const uint64_t offset = sizeof(header) + static_cast<uint64_t>(outputY) * outputRowBytes;
    if (!output.seek64(offset)) {
      LOG_ERR("MBP", "Failed seeking temporary pixel row %d", outputY);
      ok = false;
    } else {
      ok = writeExact(output, outputRow, outputRowBytes, "pixel row");
    }
  }

  if (cancellation.requested()) ok = false;
  if (ok && !output.sync()) {
    LOG_ERR("MBP", "Failed syncing temporary pixel output");
    ok = false;
  }
  if (!closeFile(output, "temporary pixel output")) ok = false;
  if (!closeFile(input, "BMP source")) ok = false;
  if (cancellation.requested()) ok = false;
  if (!ok) removePartial(temporaryPath);
  return ok;
}

}  // namespace manga
