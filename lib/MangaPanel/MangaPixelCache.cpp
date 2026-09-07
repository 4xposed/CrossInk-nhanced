#include "MangaPixelCache.h"

#include <Logging.h>
#include <uzlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace manga {
namespace {
constexpr uint8_t kMagic[4] = {'M', 'P', 'X', '1'};
constexpr uint16_t kVersion = 1;
constexpr uint16_t kMaxTargetAxis = 2048;
constexpr uint16_t kMaxSourceWidth = 4096;
constexpr uint16_t kMaxSourceHeight = 3072;
constexpr uint64_t kMaxSourcePixels = uint64_t(2048) * 3072;
constexpr size_t kRawHeaderSize = 4;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
void putU64(uint8_t* p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(v >> (i * 8));
}
uint16_t getU16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t getU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (i * 8);
  return v;
}
bool readExact(FsFile& file, void* data, size_t size) { return file.read(data, size) == static_cast<int>(size); }
bool validIdentity(const PixelIdentity& i) {
  return i.sourceSize > 0 && i.sourceWidth > 0 && i.sourceHeight > 0 && i.sourceWidth <= kMaxSourceWidth &&
         i.sourceHeight <= kMaxSourceHeight && uint64_t(i.sourceWidth) * i.sourceHeight <= kMaxSourcePixels &&
         i.width > 0 && i.height > 0 && i.width <= kMaxTargetAxis && i.height <= kMaxTargetAxis && i.screenWidth > 0 &&
         i.screenHeight > 0 && i.screenWidth <= kMaxTargetAxis && i.screenHeight <= kMaxTargetAxis &&
         i.x <= i.screenWidth && i.y <= i.screenHeight && i.width <= i.screenWidth - i.x &&
         i.height <= i.screenHeight - i.y && i.orientation < 4 && (i.flags & ~kPixelPolicyAllowedMask) == 0;
}
uint64_t rowBytes(const PixelIdentity& i) { return (uint64_t(i.width) + 3) / 4; }
uint64_t payloadBytes(const PixelIdentity& i) { return rowBytes(i) * i.height; }
// Metadata is 48 bytes: the first 44 bytes are identity plus reserved padding,
// followed by the payload checksum. Keep parsing explicit to avoid ABI padding.
constexpr size_t kEnvelopeSize = 48;
void encodeEnvelope(const PixelIdentity& i, uint32_t payloadCrc, uint8_t* out) {
  memset(out, 0, kEnvelopeSize);
  memcpy(out, kMagic, 4);
  putU16(out + 4, kVersion);
  putU16(out + 6, kEnvelopeSize);
  putU32(out + 8, i.sourcePathCrc);
  putU32(out + 12, i.sourceCrc);
  putU64(out + 16, i.sourceSize);
  const uint16_t fields[] = {i.sourceWidth, i.sourceHeight, i.width, i.height, i.x, i.y, i.screenWidth, i.screenHeight};
  for (unsigned n = 0; n < 8; ++n) putU16(out + 24 + n * 2, fields[n]);
  out[40] = i.orientation;
  out[41] = i.flags;
  putU32(out + 44, payloadCrc);
}
bool decodeEnvelope(const uint8_t* in, PixelIdentity& i, uint32_t& crc) {
  if (memcmp(in, kMagic, 4) || getU16(in + 4) != kVersion || getU16(in + 6) != kEnvelopeSize || in[42] || in[43])
    return false;
  i.sourcePathCrc = getU32(in + 8);
  i.sourceCrc = getU32(in + 12);
  i.sourceSize = getU64(in + 16);
  uint16_t* fields[] = {&i.sourceWidth, &i.sourceHeight, &i.width,       &i.height, &i.x,
                        &i.y,           &i.screenWidth,  &i.screenHeight};
  for (unsigned n = 0; n < 8; ++n) *fields[n] = getU16(in + 24 + n * 2);
  i.orientation = in[40];
  i.flags = in[41];
  crc = getU32(in + 44);
  return validIdentity(i);
}
bool sameIdentity(const PixelIdentity& a, const PixelIdentity& b) {
  return a.sourcePathCrc == b.sourcePathCrc && a.sourceCrc == b.sourceCrc && a.sourceSize == b.sourceSize &&
         a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight && a.width == b.width &&
         a.height == b.height && a.x == b.x && a.y == b.y && a.screenWidth == b.screenWidth &&
         a.screenHeight == b.screenHeight && a.orientation == b.orientation && a.flags == b.flags;
}
bool buildPaths(const char* bookFolder, uint32_t page, int16_t panel, char* finalPath, size_t finalCapacity,
                char* identityPath, size_t identityCapacity, char* directory, size_t directoryCapacity) {
  if (!bookFolder || !bookFolder[0] || page >= 10000 || panel < -1 || panel >= 255) return false;
  const uint32_t folderCrc = uzlib_crc32(bookFolder, static_cast<unsigned>(strlen(bookFolder)), 0);
  if (directory) {
    const int length =
        snprintf(directory, directoryCapacity, "/.crosspoint/manga_%lu", static_cast<unsigned long>(folderCrc));
    if (length < 0 || static_cast<size_t>(length) >= directoryCapacity) return false;
  }
  if (finalPath) {
    const int length =
        snprintf(finalPath, finalCapacity, "/.crosspoint/manga_%lu/pixels_v1_p%lu_%d.pxc",
                 static_cast<unsigned long>(folderCrc), static_cast<unsigned long>(page), static_cast<int>(panel));
    if (length < 0 || static_cast<size_t>(length) >= finalCapacity) return false;
  }
  if (identityPath) {
    const int length =
        snprintf(identityPath, identityCapacity, "/.crosspoint/manga_%lu/pixels_v1_p%lu_%d.pxc.id",
                 static_cast<unsigned long>(folderCrc), static_cast<unsigned long>(page), static_cast<int>(panel));
    if (length < 0 || static_cast<size_t>(length) >= identityCapacity) return false;
  }
  return true;
}
void removeIfPresent(const char* path) {
  if (path[0] && Storage.exists(path) && !Storage.remove(path)) LOG_ERR("MPX", "Cannot remove %s", path);
}
bool checksumPayload(FsFile& file, uint64_t bytes, uint32_t& crc, CooperativeCancellation cancellation) {
  uint8_t scratch[256];
  crc = 0;
  while (bytes) {
    if (cancellation.requested()) return false;
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(bytes, sizeof(scratch)));
    if (!readExact(file, scratch, wanted)) return false;
    crc = uzlib_crc32(scratch, static_cast<unsigned>(wanted), crc);
    bytes -= wanted;
  }
  return !cancellation.requested();
}
bool validateRaw(FsFile& file, const PixelIdentity& identity, uint32_t& crc, CooperativeCancellation cancellation) {
  if (cancellation.requested()) return false;
  const uint64_t payload = payloadBytes(identity);
  if (file.fileSize64() != kRawHeaderSize + payload || !file.seek64(0)) return false;
  uint8_t header[kRawHeaderSize];
  return readExact(file, header, sizeof(header)) && getU16(header) == identity.width &&
         getU16(header + 2) == identity.height && checksumPayload(file, payload, crc, cancellation);
}
}  // namespace

bool fingerprintImage(const char* sourcePath, PixelIdentity& identity, CooperativeCancellation cancellation) {
  if (cancellation.requested() || !sourcePath || !sourcePath[0]) return false;
  FsFile source;
  if (!Storage.openFileForRead("MPX", sourcePath, source)) return false;
  const uint64_t size = source.fileSize64();
  uint32_t crc = 0;
  const bool read = size > 0 && checksumPayload(source, size, crc, cancellation);
  const bool closed = source.close();
  if (!read || !closed || cancellation.requested()) {
    LOG_ERR("MPX", "Cannot fingerprint manga image");
    return false;
  }
  identity.sourcePathCrc = uzlib_crc32(sourcePath, static_cast<unsigned>(strlen(sourcePath)), 0);
  identity.sourceCrc = crc;
  identity.sourceSize = size;
  return true;
}

MangaPixelCache::~MangaPixelCache() { close(); }

bool MangaPixelCache::sourceIdentity(const std::string& bookFolder, uint32_t page, int16_t panel, PixelIdentity& out,
                                     CooperativeCancellation cancellation) {
  if (cancellation.requested()) return false;
  char identityPath[128];
  if (!buildPaths(bookFolder.c_str(), page, panel, nullptr, 0, identityPath, sizeof(identityPath), nullptr, 0))
    return false;
  FsFile metadata;
  if (!Storage.openFileForRead("MPX", identityPath, metadata)) return false;
  uint8_t envelope[kEnvelopeSize];
  PixelIdentity found{};
  uint32_t payloadCrc = 0;
  const bool valid = metadata.fileSize64() == sizeof(envelope) && readExact(metadata, envelope, sizeof(envelope)) &&
                     decodeEnvelope(envelope, found, payloadCrc);
  const bool closed = metadata.close();
  if (!valid || !closed || cancellation.requested()) return false;
  out = found;
  return true;
}

bool MangaPixelCache::configure(const char* bookFolder, uint32_t page, int16_t panel, const PixelIdentity& identity,
                                CooperativeCancellation cancellation) {
  close();
  configured_ = false;
  finalPath_[0] = temporaryPath_[0] = identityPath_[0] = identityTemporaryPath_[0] = '\0';
  if (cancellation.requested() || !validIdentity(identity)) return false;
  char directory[64];
  if (!buildPaths(bookFolder, page, panel, finalPath_, sizeof(finalPath_), identityPath_, sizeof(identityPath_),
                  directory, sizeof(directory)))
    return false;
  if (snprintf(temporaryPath_, sizeof(temporaryPath_), "%s.tmp", finalPath_) >= int(sizeof(temporaryPath_)) ||
      snprintf(identityTemporaryPath_, sizeof(identityTemporaryPath_), "%s.tmp", identityPath_) >=
          int(sizeof(identityTemporaryPath_)))
    return false;
  Storage.mkdir("/.crosspoint");
  if (!Storage.mkdir(directory) && !Storage.exists(directory)) return false;
  identity_ = identity;
  configured_ = true;
  return true;
}

bool MangaPixelCache::open(CooperativeCancellation cancellation) {
  close();
  if (cancellation.requested() || !configured_ || !Storage.exists(finalPath_) || !Storage.exists(identityPath_))
    return false;
  FsFile metadata;
  if (!Storage.openFileForRead("MPX", identityPath_, metadata)) return false;
  uint8_t envelope[kEnvelopeSize];
  PixelIdentity found{};
  uint32_t wantedCrc = 0;
  const bool metadataOk = metadata.fileSize64() == sizeof(envelope) &&
                          readExact(metadata, envelope, sizeof(envelope)) &&
                          decodeEnvelope(envelope, found, wantedCrc) && sameIdentity(found, identity_);
  const bool metadataClosed = metadata.close();
  if (!metadataOk || !metadataClosed || cancellation.requested() ||
      !Storage.openFileForRead("MPX", finalPath_, reader_))
    return false;
  uint32_t foundCrc = 0;
  if (!validateRaw(reader_, identity_, foundCrc, cancellation) || foundCrc != wantedCrc || !rewind()) {
    close();
    return false;
  }
  return true;
}

bool MangaPixelCache::publish(CooperativeCancellation cancellation) {
  close();
  if (!configured_ || cancellation.requested()) {
    discardTemporary();
    return false;
  }
  FsFile raw;
  uint32_t crc = 0;
  if (!Storage.openFileForRead("MPX", temporaryPath_, raw)) {
    discardTemporary();
    return false;
  }
  const bool rawOk = validateRaw(raw, identity_, crc, cancellation) && raw.sync();
  const bool rawClosed = raw.close();
  if (!rawOk || !rawClosed || cancellation.requested()) {
    discardTemporary();
    return false;
  }
  uint8_t envelope[kEnvelopeSize];
  encodeEnvelope(identity_, crc, envelope);
  FsFile metadata;
  if (!Storage.openFileForWrite("MPX", identityTemporaryPath_, metadata)) {
    discardTemporary();
    return false;
  }
  const bool metaOk = metadata.write(envelope, sizeof(envelope)) == sizeof(envelope) && metadata.sync();
  const bool metaClosed = metadata.close();
  if (!metaOk || !metaClosed || cancellation.requested()) {
    discardTemporary();
    return false;
  }
  // Finish this bounded pair replacement without polling: never acknowledge
  // cancellation between renames and leave a partially published pair.
  removeIfPresent(finalPath_);
  removeIfPresent(identityPath_);
  if (!Storage.rename(temporaryPath_, finalPath_) || !Storage.rename(identityTemporaryPath_, identityPath_)) {
    discardTemporary();
    removeIfPresent(finalPath_);
    removeIfPresent(identityPath_);
    return false;
  }
  return true;
}

void MangaPixelCache::discardTemporary() {
  close();
  removeIfPresent(temporaryPath_);
  removeIfPresent(identityTemporaryPath_);
}

bool MangaPixelCache::readRow(uint8_t* out, size_t capacity) {
  const size_t bytes = static_cast<size_t>(rowBytes(identity_));
  if (!reader_ || !out || capacity < bytes || nextRow_ >= identity_.height) return false;
  if (!readExact(reader_, out, bytes)) {
    close();
    return false;
  }
  ++nextRow_;
  return true;
}

bool MangaPixelCache::rewind() {
  if (!reader_ || !reader_.seek64(kRawHeaderSize)) return false;
  nextRow_ = 0;
  return true;
}

void MangaPixelCache::close() {
  reader_.close();
  nextRow_ = 0;
}

}  // namespace manga
