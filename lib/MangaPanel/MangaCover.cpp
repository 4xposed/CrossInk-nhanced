#include "MangaCover.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <uzlib.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "MangaBook.h"

namespace manga {
namespace {
constexpr int kMaxThumbnailAxis = 800;
// Disposable POD cache uses the documented ESP32 little-endian representation.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
constexpr uint32_t kIdentityMagic = 0x3347434d;  // MCG3
struct Identity {
  uint32_t magic, pathCrc, sourceCrc, width, height, outputWidth, outputHeight, bmpCrc;
  uint64_t sourceSize;
};
static_assert(sizeof(Identity) == 40 && offsetof(Identity, sourceSize) == 32);
static_assert(offsetof(Identity, outputWidth) == 20 && offsetof(Identity, bmpCrc) == 28);

bool normalize(int& width, int height) {
  if (height <= 0 || height > kMaxThumbnailAxis) return false;
  if (width == 0) width = (height * 2 + 1) / 3;
  return width > 0 && width <= kMaxThumbnailAxis;
}
bool endsWithIgnoreCase(const char* path, const char* suffix) {
  const size_t a = strlen(path), b = strlen(suffix);
  if (a < b) return false;
  for (size_t i = 0; i < b; ++i)
    if (std::tolower(static_cast<unsigned char>(path[a - b + i])) != suffix[i]) return false;
  return true;
}
bool readExact(FsFile& file, void* data, size_t size) { return file.read(data, size) == static_cast<int>(size); }
bool validBmp(const char* path, int width, int height, bool* ioFailed = nullptr) {
  if (!Storage.exists(path)) return false;
  FsFile file;
  if (!Storage.openFileForRead("MCV", path, file)) {
    if (ioFailed) *ioFailed = true;
    return false;
  }
  BmpHeader header{};
  if (!readExact(file, &header, sizeof(header))) {
    if (!file.close() && ioFailed) *ioFailed = true;
    return false;
  }
  const int64_t actualW = header.infoHeader.biWidth;
  const int64_t actualH =
      header.infoHeader.biHeight < 0 ? -int64_t(header.infoHeader.biHeight) : header.infoHeader.biHeight;
  const bool dimensions = actualW == width && actualH == height;
  const uint64_t expected = sizeof(BmpHeader) + uint64_t((actualW + 31) / 32 * 4) * actualH;
  const bool headerValid =
      dimensions && header.fileHeader.bfType == 0x4d42 && header.fileHeader.bfSize == expected &&
      header.fileHeader.bfOffBits == sizeof(BmpHeader) && header.infoHeader.biSize == sizeof(header.infoHeader) &&
      header.infoHeader.biPlanes == 1 && header.infoHeader.biBitCount == 1 && header.infoHeader.biCompression == 0 &&
      header.infoHeader.biSizeImage == expected - sizeof(BmpHeader) && header.infoHeader.biClrUsed == 2;
  const bool valid = headerValid && file.fileSize64() == expected;
  const bool closed = file.close();
  if (!closed && ioFailed) *ioFailed = true;
  return closed && valid;
}
bool sourceCrc(FsFile& file, const uint64_t size, uint32_t& crc, CooperativeCancellation cancellation) {
  crc = 0;
  uint8_t buffer[256];
  uint64_t remaining = size;
  while (remaining) {
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buffer)));
    if (file.read(buffer, wanted) != static_cast<int>(wanted)) return false;
    crc = uzlib_crc32(buffer, static_cast<unsigned>(wanted), crc);
    remaining -= wanted;
    if (cancellation.requested()) return false;
  }
  return true;
}
bool readIdentity(const char* path, Identity& found, bool& ioFailed) {
  if (!Storage.exists(path)) return false;
  FsFile file;
  if (!Storage.openFileForRead("MCV", path, file)) {
    ioFailed = true;
    return false;
  }
  const bool valid =
      file.fileSize64() == sizeof(found) && readExact(file, &found, sizeof(found)) && found.magic == kIdentityMagic;
  const bool closed = file.close();
  if (!closed) ioFailed = true;
  return closed && valid;
}
bool bmpChecksum(const char* path, uint32_t& crc, CooperativeCancellation cancellation, bool& ioFailed) {
  if (cancellation.requested()) return false;
  FsFile file;
  if (!Storage.openFileForRead("MCV", path, file)) {
    ioFailed = true;
    return false;
  }
  const bool valid = sourceCrc(file, file.fileSize64(), crc, cancellation);
  const bool closed = file.close();
  if (!closed || (!valid && !cancellation.requested())) ioFailed = true;
  return valid && closed;
}
// Match the existing converters' adaptive contain policy and float truncation.
// Progressive JPEG geometry uses its actual eighth-scale decoded dimensions.
bool emittedDimensions(const BmpConversionDimensions& source, int width, int height, bool bitmap, uint32_t& outWidth,
                       uint32_t& outHeight) {
  outWidth = width;
  outHeight = height;
  if (bitmap) return true;  // BMP conversion letterboxes into the requested canvas.
  if (source.width <= 0 || source.height <= 0) return false;
  const int sw = source.progressive ? (source.width + 7) / 8 : source.width;
  const int sh = source.progressive ? (source.height + 7) / 8 : source.height;
  const int64_t cross = int64_t(sw) * height - int64_t(width) * sh;
  const int64_t difference = cross < 0 ? -cross : cross;
  if (difference * 100 > int64_t(width) * sh * 18) {
    const float scale = std::min(float(width) / sw, float(height) / sh);
    outWidth = std::max(1, int(sw * scale));
    outHeight = std::max(1, int(sh * scale));
  }
  return outWidth <= unsigned(width) && outHeight <= unsigned(height);
}
bool writeIdentity(const char* path, const Identity& identity, CooperativeCancellation cancellation) {
  FsFile file;
  if (!Storage.openFileForWrite("MCV", path, file)) return false;
  const bool ok =
      file.write(&identity, sizeof(identity)) == sizeof(identity) && !cancellation.requested() && file.sync();
  return file.close() && ok;
}
void cleanup(const char* path) {
  if (Storage.exists(path) && !Storage.remove(path)) LOG_ERR("MCV", "Cannot remove %s", path);
}
bool convertBmp(FsFile& source, FsFile& output, int targetWidth, int targetHeight,
                CooperativeCancellation cancellation) {
  Bitmap bitmap(source);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  const int sourceWidth = bitmap.getWidth(), sourceHeight = bitmap.getHeight();
  const int scaledWidth = std::max(1, std::min(targetWidth, sourceWidth * targetHeight / sourceHeight));
  const int scaledHeight = std::max(1, std::min(targetHeight, sourceHeight * targetWidth / sourceWidth));
  const int xOffset = (targetWidth - scaledWidth) / 2;
  const int yOffset = (targetHeight - scaledHeight) / 2;
  const size_t rawBytes = bitmap.getRowBytes(), grayBytes = (sourceWidth + 3) / 4;
  const size_t outputBytes = (targetWidth + 31) / 32 * 4;
  // Dynamic because a valid source row can be up to 8192 bytes; task stacks are small.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(rawBytes + grayBytes + outputBytes);
  if (!scratch) {
    LOG_ERR("MCV", "OOM for BMP row buffers (%u bytes)", unsigned(rawBytes + grayBytes + outputBytes));
    return false;
  }
  uint8_t* raw = scratch.get();
  uint8_t* gray = raw + rawBytes;
  uint8_t* out = gray + grayBytes;
  BmpHeader header;
  createBmpHeader(&header, targetWidth, targetHeight,
                  bitmap.isTopDown() ? BmpRowOrder::TopDown : BmpRowOrder::BottomUp);
  if (output.write(&header, sizeof(header)) != sizeof(header)) return false;
  int loadedY = -1;
  for (int y = 0; y < targetHeight; ++y) {
    if (cancellation.requested()) return false;
    memset(out, 0xff, outputBytes);
    if (y >= yOffset && y < yOffset + scaledHeight) {
      const int wantedY = (y - yOffset) * sourceHeight / scaledHeight;
      while (loadedY < wantedY) {
        if (cancellation.requested()) return false;
        if (bitmap.readNextRow(gray, raw) != BmpReaderError::Ok) return false;
        ++loadedY;
      }
      for (int x = 0; x < scaledWidth; ++x) {
        const int sourceX = x * sourceWidth / scaledWidth;
        const uint8_t level = (gray[sourceX >> 2] >> (6 - ((sourceX & 3) * 2))) & 3;
        if (level < 2) out[(x + xOffset) >> 3] &= ~(0x80 >> ((x + xOffset) & 7));
      }
    }
    if (output.write(out, outputBytes) != outputBytes) return false;
  }
  return true;
}
}  // namespace

std::string cachePath(const std::string& folder) {
  return "/.crosspoint/manga_" + std::to_string(uzlib_crc32(folder.data(), static_cast<unsigned>(folder.size()), 0));
}
std::string thumbnailTemplatePath(const std::string& folder) {
  return cachePath(folder) + "/thumb_v3_[WIDTH]x[HEIGHT].bmp";
}
std::string thumbnailPath(const std::string& folder, int width, int height) {
  if (!normalize(width, height)) return {};
  return cachePath(folder) + "/thumb_v3_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}
bool fitThumbnailDimensions(const int sourceWidth, const int sourceHeight, const int maxWidth, const int maxHeight,
                            int& width, int& height) {
  width = height = 0;
  if (sourceWidth <= 0 || sourceHeight <= 0 || maxWidth <= 0 || maxHeight <= 0 || maxWidth > kMaxThumbnailAxis ||
      maxHeight > kMaxThumbnailAxis)
    return false;
  if (int64_t(sourceWidth) * maxHeight > int64_t(sourceHeight) * maxWidth) {
    width = maxWidth;
    height = std::max(1, static_cast<int>(int64_t(sourceHeight) * maxWidth / sourceWidth));
  } else {
    height = maxHeight;
    width = std::max(1, static_cast<int>(int64_t(sourceWidth) * maxHeight / sourceHeight));
  }
  return true;
}
namespace {
// Cache basenames use only a uint32 hash and bounded dimensions. Six transaction
// names plus the directory exceed a task stack budget; allocate once per attempt.
struct CoverPaths {
  char directory[48], output[96], identity[100], temporary[100], identityTemporary[104], backup[100],
      identityBackup[104];
  explicit CoverPaths(const std::string& folder, int width, int height) {
    const auto hash = uzlib_crc32(folder.data(), static_cast<unsigned>(folder.size()), 0);
    snprintf(directory, sizeof(directory), "/.crosspoint/manga_%u", unsigned(hash));
    snprintf(output, sizeof(output), "%s/thumb_v3_%dx%d.bmp", directory, width, height);
    snprintf(identity, sizeof(identity), "%s.src", output);
    snprintf(temporary, sizeof(temporary), "%s.tmp", output);
    snprintf(identityTemporary, sizeof(identityTemporary), "%s.tmp", identity);
    snprintf(backup, sizeof(backup), "%s.bak", output);
    snprintf(identityBackup, sizeof(identityBackup), "%s.bak", identity);
  }
};
bool validPair(const char* image, const char* sidecar, int width, int height, bool& ioFailed,
               CooperativeCancellation cancellation, const Identity* wanted = nullptr) {
  if (cancellation.requested()) return false;
  Identity found{};
  if (!readIdentity(sidecar, found, ioFailed) || found.width != unsigned(width) || found.height != unsigned(height) ||
      !found.outputWidth || !found.outputHeight || found.outputWidth > unsigned(width) ||
      found.outputHeight > unsigned(height))
    return false;
  if (wanted && (found.pathCrc != wanted->pathCrc || found.sourceCrc != wanted->sourceCrc ||
                 found.sourceSize != wanted->sourceSize))
    return false;
  if (!validBmp(image, found.outputWidth, found.outputHeight, &ioFailed)) return false;
  uint32_t crc = 0;
  return bmpChecksum(image, crc, cancellation, ioFailed) && crc == found.bmpCrc;
}
bool move(const char* from, const char* to) {
  if (Storage.rename(from, to)) return true;
  LOG_ERR("MCV", "Cover rename failed: %s -> %s", from, to);
  return false;
}
void recover(const CoverPaths& p, int width, int height, bool& ioFailed, CooperativeCancellation cancellation) {
  if (validPair(p.output, p.identity, width, height, ioFailed, cancellation) || ioFailed || cancellation.requested())
    return;
  // A failed backup or rollback rename can leave either half at its primary name.
  // Only a digest-bound candidate authorizes removing/replacing the other names.
  const char* images[] = {p.backup, p.backup, p.output};
  const char* identities[] = {p.identityBackup, p.identity, p.identityBackup};
  for (int i = 0; i < 3; ++i) {
    const bool valid = validPair(images[i], identities[i], width, height, ioFailed, cancellation);
    if (ioFailed || cancellation.requested()) return;
    if (!valid) continue;
    // Short masked rename only: all candidate CRC work has completed and handles closed.
    if (images[i] != p.output) {
      cleanup(p.output);
      if (!move(images[i], p.output)) {
        ioFailed = true;
        return;
      }
    }
    if (identities[i] != p.identity) {
      cleanup(p.identity);
      if (!move(identities[i], p.identity)) {
        ioFailed = true;
        return;
      }
    }
    return;
  }
}
bool promote(const CoverPaths& p, const bool previous) {
  cleanup(p.backup);
  cleanup(p.identityBackup);
  if (previous) {
    if (!move(p.output, p.backup)) return false;
    if (!move(p.identity, p.identityBackup)) {
      move(p.backup, p.output);
      return false;
    }
  } else {
    cleanup(p.output);
    cleanup(p.identity);
  }
  const bool imagePromoted = move(p.temporary, p.output);
  if (!imagePromoted || !move(p.identityTemporary, p.identity)) {
    if (imagePromoted) cleanup(p.output);
    if (previous) {
      const bool imageRestored = move(p.backup, p.output);
      const bool identityRestored = move(p.identityBackup, p.identity);
      // Retain a complete backup if recovery itself fails.
      if (imageRestored && !identityRestored) move(p.output, p.backup);
      if (!imageRestored && identityRestored) move(p.identity, p.identityBackup);
    }
    return false;
  }
  cleanup(p.backup);
  cleanup(p.identityBackup);
  return true;
}
struct CancellationLatch {
  CooperativeCancellation source;
  bool cancelled = false;
  static bool poll(void* context) {
    auto& self = *static_cast<CancellationLatch*>(context);
    self.cancelled = self.cancelled || self.source.requested();
    return self.cancelled;
  }
};
}  // namespace

ThumbnailResult generateThumbnailControlled(MangaBook& book, const std::string& folder, int width, int height,
                                            CooperativeCancellation cancellation, ThumbnailDiagnostics* diagnostics) {
  CancellationLatch latch{cancellation};
  cancellation = {CancellationLatch::poll, &latch};
  if (diagnostics) *diagnostics = {};
  auto finish = [&](ThumbnailResult result) {
    if (diagnostics) diagnostics->result = result;
    return result;
  };
  auto failure = [&] { return finish(latch.cancelled ? ThumbnailResult::Cancelled : ThumbnailResult::Failed); };
  auto stage = [&](ThumbnailStage value) {
    if (diagnostics) diagnostics->stage = value;
  };
  stage(ThumbnailStage::Validation);
  if (cancellation.requested()) return failure();
  if (!normalize(width, height) || folder.empty() || folder.size() > std::numeric_limits<size_t>::max() - 258) {
    LOG_ERR("MCV", "Invalid manga thumbnail request");
    return failure();
  }
  auto paths = makeUniqueNoThrow<CoverPaths>(folder, width, height);
  auto sourcePath = makeUniqueNoThrow<char[]>(folder.size() + 258);
  if (!paths || !sourcePath) {
    LOG_ERR("MCV", "OOM for cover transaction (%u + %u bytes)", unsigned(sizeof(CoverPaths)),
            unsigned(folder.size() + 258));
    return failure();
  }
  const auto& p = *paths;
  bool ioFailed = false;
  recover(p, width, height, ioFailed, cancellation);
  if (ioFailed || cancellation.requested()) {
    LOG_ERR("MCV", "Cover recovery stopped");
    return failure();
  }
  if (book.pageImagePath(0, sourcePath.get(), folder.size() + 258, cancellation) != PathResult::Found) {
    LOG_ERR("MCV", "Manga cover page is missing");
    return failure();
  }
  if (cancellation.requested()) return failure();
  FsFile source;
  if (!Storage.openFileForRead("MCV", sourcePath.get(), source)) return failure();
  Identity identity{};
  identity.magic = kIdentityMagic;
  identity.pathCrc = uzlib_crc32(sourcePath.get(), static_cast<unsigned>(strlen(sourcePath.get())), 0);
  identity.width = static_cast<uint32_t>(width);
  identity.height = static_cast<uint32_t>(height);
  identity.sourceSize = source.fileSize64();
  if (diagnostics) {
    diagnostics->sourceSize = identity.sourceSize;
    diagnostics->width = width;
    diagnostics->height = height;
    diagnostics->sourceType = endsWithIgnoreCase(sourcePath.get(), ".png")
                                  ? "PNG"
                                  : (endsWithIgnoreCase(sourcePath.get(), ".bmp") ? "BMP" : "JPEG");
  }
  stage(ThumbnailStage::Crc);
  const bool checksum = sourceCrc(source, identity.sourceSize, identity.sourceCrc, cancellation);
  const bool sourceClosed = source.close();
  if (!checksum || !sourceClosed) {
    LOG_ERR("MCV", "Cover source validation stopped");
    return failure();
  }
  if (diagnostics) diagnostics->sourceCrc = identity.sourceCrc;
  if (cancellation.requested()) return failure();
  const bool cached = validPair(p.output, p.identity, width, height, ioFailed, cancellation, &identity);
  if (ioFailed || cancellation.requested()) {
    LOG_ERR("MCV", "Cover cache validation stopped");
    return failure();
  }
  if (cached) return finish(ThumbnailResult::Cached);
  Storage.mkdir("/.crosspoint");
  if (!Storage.mkdir(p.directory) && !Storage.exists(p.directory)) {
    LOG_ERR("MCV", "Cannot create cover cache");
    return failure();
  }
  cleanup(p.temporary);
  cleanup(p.identityTemporary);
  const ScopedCleanup removeTemps{[&] {
    cleanup(p.temporary);
    cleanup(p.identityTemporary);
  }};
  if (cancellation.requested()) return failure();
  if (!Storage.openFileForRead("MCV", sourcePath.get(), source)) return failure();
  FsFile output;
  if (!Storage.openFileForWrite("MCV", p.temporary, output)) {
    source.close();
    return failure();
  }
  stage(ThumbnailStage::Conversion);
  bool converted = false;
  BmpConversionDimensions sourceDimensions;
  if (endsWithIgnoreCase(sourcePath.get(), ".jpg") || endsWithIgnoreCase(sourcePath.get(), ".jpeg"))
    converted = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source, output, width, height, true, cancellation,
                                                                    &sourceDimensions);
  else if (endsWithIgnoreCase(sourcePath.get(), ".png"))
    converted = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source, output, width, height, true, cancellation,
                                                                  &sourceDimensions);
  else if (endsWithIgnoreCase(sourcePath.get(), ".bmp"))
    converted = convertBmp(source, output, width, height, cancellation);
  if (diagnostics) diagnostics->sourceDimensions = sourceDimensions;
  if (converted) stage(ThumbnailStage::Sync);
  const bool synced = converted && !cancellation.requested() && output.sync();
  const bool outputClosed = output.close();
  const bool inputClosed = source.close();
  if (!synced || !outputClosed || !inputClosed ||
      !emittedDimensions(sourceDimensions, width, height, endsWithIgnoreCase(sourcePath.get(), ".bmp"),
                         identity.outputWidth, identity.outputHeight) ||
      !validBmp(p.temporary, identity.outputWidth, identity.outputHeight) ||
      !bmpChecksum(p.temporary, identity.bmpCrc, cancellation, ioFailed)) {
    LOG_ERR("MCV", "Manga thumbnail conversion/finalization stopped");
    return failure();
  }
  stage(ThumbnailStage::Identity);
  if (cancellation.requested() || !writeIdentity(p.identityTemporary, identity, cancellation)) return failure();
  stage(ThumbnailStage::Publication);
  const bool previous = validPair(p.output, p.identity, width, height, ioFailed, cancellation);
  if (ioFailed || cancellation.requested()) return failure();
  // Mask cancellation only during this short rename/rollback transaction. No
  // decoder or CRC work remains; all handles are closed before promotion starts.
  if (!promote(p, previous)) return failure();
  stage(ThumbnailStage::Complete);
  return finish(ThumbnailResult::Published);
}
bool generateThumbnail(MangaBook& book, const std::string& folder, int width, int height, bool* regenerated) {
  const auto result = generateThumbnailControlled(book, folder, width, height);
  if (regenerated) *regenerated = result == ThumbnailResult::Published;
  return result == ThumbnailResult::Published || result == ThumbnailResult::Cached;
}
}  // namespace manga
