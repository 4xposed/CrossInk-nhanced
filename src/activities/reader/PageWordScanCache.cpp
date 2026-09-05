#include "PageWordScanCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {
constexpr char TAG[] = "WLS";
constexpr uint32_t FNV1A_OFFSET = UINT32_C(2166136261);
constexpr uint32_t FNV1A_PRIME = UINT32_C(16777619);
constexpr size_t MAX_CACHE_PATH_BYTES = 512;
static_assert(static_cast<size_t>(UINT16_MAX) * PageWordScanCache::kRecordSize <=
                  std::numeric_limits<size_t>::max() - PageWordScanCache::kHeaderSize,
              "Every representable wlscan payload length must fit size_t");

struct CacheHeader {
  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t backend = 0;
  uint16_t flags = 0;
  uint16_t spine = 0;
  uint16_t page = 0;
  uint32_t glyphHash = 0;
  uint64_t dictionarySignature = 0;
  uint16_t candidateCount = 0;
  uint16_t cursor = 0;
  uint32_t payloadFnv1a = 0;
};

struct SiblingPaths {
  std::unique_ptr<char[]> storage;
  const char* finalPath = nullptr;
  char* tmpPath = nullptr;
  char* backupPath = nullptr;
};

struct CandidateOrder {
  bool havePrevious = false;
  uint16_t firstGlyph = 0;
  uint16_t firstPageWord = 0;
  uint16_t lastPageWord = 0;
};

void putLe16(uint8_t* destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void putLe32(uint8_t* destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

void putLe64(uint8_t* destination, const uint64_t value) {
  putLe32(destination, static_cast<uint32_t>(value));
  putLe32(destination + 4, static_cast<uint32_t>(value >> 32));
}

uint16_t getLe16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8);
}

uint32_t getLe32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) | (static_cast<uint32_t>(source[3]) << 24);
}

uint64_t getLe64(const uint8_t* source) {
  return static_cast<uint64_t>(getLe32(source)) | (static_cast<uint64_t>(getLe32(source + 4)) << 32);
}

uint32_t fnv1a(const uint8_t* bytes, const size_t count, uint32_t hash = FNV1A_OFFSET) {
  for (size_t index = 0; index < count; ++index) {
    hash ^= bytes[index];
    hash *= FNV1A_PRIME;
  }
  return hash;
}

void encodeHeader(const CacheHeader& header, uint8_t (&bytes)[PageWordScanCache::kHeaderSize]) {
  putLe32(bytes, header.magic);
  bytes[4] = header.version;
  bytes[5] = header.backend;
  putLe16(bytes + 6, header.flags);
  putLe16(bytes + 8, header.spine);
  putLe16(bytes + 10, header.page);
  putLe32(bytes + 12, header.glyphHash);
  putLe64(bytes + 16, header.dictionarySignature);
  putLe16(bytes + 24, header.candidateCount);
  putLe16(bytes + 26, header.cursor);
  putLe32(bytes + 28, header.payloadFnv1a);
}

CacheHeader decodeHeader(const uint8_t (&bytes)[PageWordScanCache::kHeaderSize]) {
  CacheHeader header;
  header.magic = getLe32(bytes);
  header.version = bytes[4];
  header.backend = bytes[5];
  header.flags = getLe16(bytes + 6);
  header.spine = getLe16(bytes + 8);
  header.page = getLe16(bytes + 10);
  header.glyphHash = getLe32(bytes + 12);
  header.dictionarySignature = getLe64(bytes + 16);
  header.candidateCount = getLe16(bytes + 24);
  header.cursor = getLe16(bytes + 26);
  header.payloadFnv1a = getLe32(bytes + 28);
  return header;
}

void encodeCandidate(const PageWordCandidate& candidate, uint8_t (&bytes)[PageWordScanCache::kRecordSize]) {
  putLe16(bytes, candidate.firstGlyph);
  bytes[2] = candidate.glyphCount;
  bytes[3] = candidate.matchedBytes;
  putLe16(bytes + 4, candidate.firstPageWord);
  putLe16(bytes + 6, candidate.lastPageWord);
}

PageWordCandidate decodeCandidate(const uint8_t (&bytes)[PageWordScanCache::kRecordSize]) {
  PageWordCandidate candidate;
  candidate.firstGlyph = getLe16(bytes);
  candidate.glyphCount = bytes[2];
  candidate.matchedBytes = bytes[3];
  candidate.firstPageWord = getLe16(bytes + 4);
  candidate.lastPageWord = getLe16(bytes + 6);
  return candidate;
}

bool validateCandidate(const PageWordCandidate& candidate, const uint16_t sourceGlyphCount, CandidateOrder& order) {
  if (candidate.glyphCount == 0 || candidate.matchedBytes == 0 ||
      candidate.matchedBytes > static_cast<size_t>(candidate.glyphCount) * 4 ||
      candidate.firstGlyph >= sourceGlyphCount || candidate.glyphCount > sourceGlyphCount - candidate.firstGlyph ||
      candidate.firstPageWord == PageTextGlyph::kSyntheticPageWord ||
      candidate.lastPageWord == PageTextGlyph::kSyntheticPageWord || candidate.firstPageWord > candidate.lastPageWord) {
    LOG_ERR(TAG, "Invalid wlscan candidate at glyph %u", candidate.firstGlyph);
    return false;
  }
  if (order.havePrevious &&
      (candidate.firstGlyph <= order.firstGlyph || candidate.firstPageWord < order.firstPageWord ||
       candidate.lastPageWord < order.lastPageWord)) {
    LOG_ERR(TAG, "Non-monotonic wlscan candidate at glyph %u", candidate.firstGlyph);
    return false;
  }
  order.havePrevious = true;
  order.firstGlyph = candidate.firstGlyph;
  order.firstPageWord = candidate.firstPageWord;
  order.lastPageWord = candidate.lastPageWord;
  return true;
}

bool makeSiblingPaths(const char* path, SiblingPaths& paths) {
  if (!path || path[0] == '\0') {
    LOG_ERR(TAG, "Missing wlscan cache path");
    return false;
  }
  size_t length = 0;
  while (length <= MAX_CACHE_PATH_BYTES && path[length] != '\0') ++length;
  if (length > MAX_CACHE_PATH_BYTES) {
    LOG_ERR(TAG, "wlscan cache path exceeds %u bytes", static_cast<unsigned>(MAX_CACHE_PATH_BYTES));
    return false;
  }
  constexpr size_t suffixBytes = 5;  // Four suffix bytes plus NUL.
  if (length > (std::numeric_limits<size_t>::max() / 2) - suffixBytes) {
    LOG_ERR(TAG, "wlscan sibling path size overflow");
    return false;
  }
  const size_t onePathBytes = length + suffixBytes;
  const size_t allocationBytes = onePathBytes * 2;
  // The caller-supplied path is runtime-sized, and two sibling names must live
  // together through replacement. This single fallible allocation is bounded
  // to 1,034 bytes and avoids abort-prone std::string growth.
  paths.storage = makeUniqueNoThrow<char[]>(allocationBytes);
  if (!paths.storage) {
    LOG_ERR(TAG, "OOM allocating %u-byte wlscan path scratch", static_cast<unsigned>(allocationBytes));
    return false;
  }
  paths.finalPath = path;
  paths.tmpPath = paths.storage.get();
  paths.backupPath = paths.tmpPath + onePathBytes;
  std::memcpy(paths.tmpPath, path, length);
  std::memcpy(paths.tmpPath + length, ".tmp", suffixBytes);
  std::memcpy(paths.backupPath, path, length);
  std::memcpy(paths.backupPath + length, ".bak", suffixBytes);
  return true;
}

bool closeFile(HalFile& file, const char* path) {
  (void)path;  // Test logging is compiled out.
  if (!file.isOpen()) return true;
  if (file.close()) return true;
  LOG_ERR(TAG, "Failed to close wlscan cache: %s", path);
  return false;
}

bool removeExisting(const char* path) {
  if (!Storage.exists(path)) return true;
  if (Storage.remove(path)) return true;
  LOG_ERR(TAG, "Failed to remove wlscan cache file: %s", path);
  return false;
}

bool recoverBackup(const SiblingPaths& paths, const bool requireStaleRemoval) {
  if (!Storage.exists(paths.backupPath)) return true;
  if (Storage.exists(paths.finalPath)) {
    if (Storage.remove(paths.backupPath)) return true;
    LOG_ERR(TAG, "Failed to remove stale wlscan backup: %s", paths.backupPath);
    return !requireStaleRemoval;
  }
  if (Storage.rename(paths.backupPath, paths.finalPath)) {
    LOG_INF(TAG, "Recovered wlscan backup: %s", paths.finalPath);
    return true;
  }
  LOG_ERR(TAG, "Failed to recover wlscan backup: %s", paths.finalPath);
  return false;
}

bool readExact(HalFile& file, void* destination, const size_t count, const char* path) {
  (void)path;  // Test logging is compiled out.
  const int read = file.read(destination, count);
  if (read == static_cast<int>(count)) return true;
  LOG_ERR(TAG, "Short wlscan read from %s: wanted %u, got %d", path, static_cast<unsigned>(count), read);
  return false;
}

bool writeExact(HalFile& file, const void* source, const size_t count, const char* path) {
  (void)path;  // Test logging is compiled out.
  const size_t written = file.write(source, count);
  if (written == count) return true;
  LOG_ERR(TAG, "Short wlscan write to %s: wanted %u, got %u", path, static_cast<unsigned>(count),
          static_cast<unsigned>(written));
  return false;
}

bool headerMatches(const CacheHeader& header, const PageWordScanCacheIdentity& identity) {
  if (header.magic != PageWordScanCache::kMagic || header.version != PageWordScanCache::kVersion ||
      header.backend != static_cast<uint8_t>(identity.backend) || header.flags != PageWordScanCache::kCompleteFlag ||
      header.spine != identity.spine || header.page != identity.page || header.glyphHash != identity.glyphHash ||
      header.dictionarySignature != identity.dictionarySignature || header.candidateCount > identity.sourceGlyphCount) {
    LOG_ERR(TAG, "Rejected stale or invalid wlscan header");
    return false;
  }
  return true;
}

bool validBackend(const DictionaryBackendKind backend) {
  switch (backend) {
    case DictionaryBackendKind::StarDict:
    case DictionaryBackendKind::Japanese:
      return true;
  }
  LOG_ERR(TAG, "Rejected unknown wlscan backend");
  return false;
}

bool validateFileStreaming(const char* path, const PageWordScanCacheIdentity& identity, CacheHeader& validatedHeader) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "Failed to open wlscan cache for validation: %s", path);
    return false;
  }

  bool valid = false;
  uint8_t headerBytes[PageWordScanCache::kHeaderSize];
  if (readExact(file, headerBytes, sizeof(headerBytes), path)) {
    const CacheHeader header = decodeHeader(headerBytes);
    const size_t payloadBytes = static_cast<size_t>(header.candidateCount) * PageWordScanCache::kRecordSize;
    const size_t expectedFileBytes = PageWordScanCache::kHeaderSize + payloadBytes;
    if (!headerMatches(header, identity)) {
      // Logged by headerMatches().
    } else if (file.fileSize() != expectedFileBytes) {
      LOG_ERR(TAG, "Rejected non-exact wlscan file length: %s", path);
    } else {
      CandidateOrder order;
      uint32_t payloadHash = FNV1A_OFFSET;
      valid = true;
      for (uint16_t index = 0; index < header.candidateCount; ++index) {
        uint8_t recordBytes[PageWordScanCache::kRecordSize];
        if (!readExact(file, recordBytes, sizeof(recordBytes), path)) {
          valid = false;
          break;
        }
        payloadHash = fnv1a(recordBytes, sizeof(recordBytes), payloadHash);
        if (!validateCandidate(decodeCandidate(recordBytes), identity.sourceGlyphCount, order)) {
          valid = false;
          break;
        }
      }
      if (valid && payloadHash != header.payloadFnv1a) {
        LOG_ERR(TAG, "Rejected wlscan payload checksum: %s", path);
        valid = false;
      }
      if (valid) validatedHeader = header;
    }
  }
  if (!closeFile(file, path)) valid = false;
  return valid;
}

bool cleanTemp(const SiblingPaths& paths) { return removeExisting(paths.tmpPath); }

bool promoteTemp(const SiblingPaths& paths) {
  if (!Storage.exists(paths.finalPath)) {
    if (Storage.rename(paths.tmpPath, paths.finalPath)) return true;
    LOG_ERR(TAG, "Failed to promote wlscan temp cache: %s", paths.finalPath);
    cleanTemp(paths);
    return false;
  }

  if (!Storage.rename(paths.finalPath, paths.backupPath)) {
    LOG_ERR(TAG, "Failed to preserve old wlscan cache before replacement");
    cleanTemp(paths);
    return false;
  }
  if (Storage.rename(paths.tmpPath, paths.finalPath)) {
    if (Storage.remove(paths.backupPath)) return true;
    LOG_ERR(TAG, "Failed to remove promoted wlscan backup: %s", paths.backupPath);
    return false;
  }

  LOG_ERR(TAG, "Failed to promote wlscan cache; restoring previous cache");
  if (!Storage.rename(paths.backupPath, paths.finalPath)) {
    LOG_ERR(TAG, "Failed to restore wlscan cache backup: %s", paths.finalPath);
  }
  cleanTemp(paths);
  return false;
}
}  // namespace

bool PageWordScanCache::save(const char* path, const PageWordScanCacheIdentity& identity,
                             const PageWordScanner& scanner, const uint16_t cursor) {
  if (!scanner.completedSuccessfully() || scanner.truncated()) {
    LOG_ERR(TAG, "Refusing to cache incomplete or truncated wlscan scan");
    return false;
  }
  return saveCandidates(path, identity, scanner.candidate(0), scanner.candidateCount(), cursor);
}

bool PageWordScanCache::saveLoaded(const char* path, const PageWordScanCacheIdentity& identity, const uint16_t cursor) {
  if ((candidateCount_ != 0 && !candidates_) || candidateCount_ > identity.sourceGlyphCount) {
    LOG_ERR(TAG, "Refusing to rewrite an invalid wlscan snapshot");
    return false;
  }
  return saveCandidates(path, identity, candidates_.get(), candidateCount_, cursor);
}

bool PageWordScanCache::saveCandidates(const char* path, const PageWordScanCacheIdentity& identity,
                                       const PageWordCandidate* candidates, const uint16_t candidateCount,
                                       const uint16_t cursor) {
  if (candidateCount > identity.sourceGlyphCount || (candidateCount != 0 && !candidates)) {
    LOG_ERR(TAG, "Refusing impossible wlscan candidate count");
    return false;
  }
  if (!validBackend(identity.backend)) return false;

  SiblingPaths paths;
  if (!makeSiblingPaths(path, paths)) return false;
  if (!removeExisting(paths.tmpPath) || !recoverBackup(paths, true)) return false;

  CandidateOrder order;
  uint32_t payloadHash = FNV1A_OFFSET;
  for (uint16_t index = 0; index < candidateCount; ++index) {
    const PageWordCandidate& item = candidates[index];
    if (!validateCandidate(item, identity.sourceGlyphCount, order)) return false;
    uint8_t bytes[kRecordSize];
    encodeCandidate(item, bytes);
    payloadHash = fnv1a(bytes, sizeof(bytes), payloadHash);
  }

  const CacheHeader header{PageWordScanCache::kMagic,
                           PageWordScanCache::kVersion,
                           static_cast<uint8_t>(identity.backend),
                           PageWordScanCache::kCompleteFlag,
                           identity.spine,
                           identity.page,
                           identity.glyphHash,
                           identity.dictionarySignature,
                           candidateCount,
                           cursor,
                           payloadHash};
  uint8_t headerBytes[kHeaderSize];
  encodeHeader(header, headerBytes);

  HalFile file;
  if (!Storage.openFileForWrite(TAG, paths.tmpPath, file)) {
    LOG_ERR(TAG, "Failed to open wlscan temp cache for writing: %s", paths.tmpPath);
    cleanTemp(paths);
    return false;
  }
  bool wrote = writeExact(file, headerBytes, sizeof(headerBytes), paths.tmpPath);
  for (uint16_t index = 0; wrote && index < candidateCount; ++index) {
    uint8_t bytes[kRecordSize];
    encodeCandidate(candidates[index], bytes);
    wrote = writeExact(file, bytes, sizeof(bytes), paths.tmpPath);
  }
  if (!closeFile(file, paths.tmpPath)) wrote = false;
  if (!wrote) {
    cleanTemp(paths);
    return false;
  }

  CacheHeader verifiedHeader;
  if (!validateFileStreaming(paths.tmpPath, identity, verifiedHeader)) {
    cleanTemp(paths);
    return false;
  }
  return promoteTemp(paths);
}

bool PageWordScanCache::load(const char* path, const PageWordScanCacheIdentity& identity) {
  clear();
  if (!validBackend(identity.backend)) return false;

  SiblingPaths paths;
  if (!makeSiblingPaths(path, paths)) return false;
  if (!removeExisting(paths.tmpPath)) {
    // A stale temporary is never considered a cache hit. Its cleanup failure
    // does not invalidate an independently complete final file.
    LOG_ERR(TAG, "Ignoring unremovable stale wlscan temp: %s", paths.tmpPath);
  }
  if (!recoverBackup(paths, false)) return false;

  HalFile file;
  if (!Storage.openFileForRead(TAG, paths.finalPath, file)) {
    LOG_ERR(TAG, "Failed to open wlscan cache: %s", paths.finalPath);
    return false;
  }

  bool valid = false;
  CacheHeader header;
  std::unique_ptr<PageWordCandidate[]> loadedCandidates;
  uint8_t headerBytes[kHeaderSize];
  if (readExact(file, headerBytes, sizeof(headerBytes), paths.finalPath)) {
    header = decodeHeader(headerBytes);
    if (headerMatches(header, identity)) {
      const size_t payloadBytes = static_cast<size_t>(header.candidateCount) * kRecordSize;
      if (file.fileSize() != kHeaderSize + payloadBytes) {
        LOG_ERR(TAG, "Rejected non-exact wlscan file length: %s", paths.finalPath);
      } else {
        if (header.candidateCount > 0) {
          // Runtime candidate counts cannot use the small task stack or static
          // storage. This exact, fallible ownership is bounded by the current
          // source at sourceGlyphCount * 8 bytes (at most 524,280 bytes).
          loadedCandidates = makeUniqueNoThrow<PageWordCandidate[]>(header.candidateCount);
          if (!loadedCandidates) {
            LOG_ERR(TAG, "OOM allocating %u-byte wlscan candidate array", static_cast<unsigned>(payloadBytes));
          }
        }

        if (header.candidateCount == 0 || loadedCandidates) {
          CandidateOrder order;
          uint32_t payloadHash = FNV1A_OFFSET;
          valid = true;
          for (uint16_t index = 0; index < header.candidateCount; ++index) {
            uint8_t recordBytes[kRecordSize];
            if (!readExact(file, recordBytes, sizeof(recordBytes), paths.finalPath)) {
              valid = false;
              break;
            }
            payloadHash = fnv1a(recordBytes, sizeof(recordBytes), payloadHash);
            const PageWordCandidate loadedCandidate = decodeCandidate(recordBytes);
            if (!validateCandidate(loadedCandidate, identity.sourceGlyphCount, order)) {
              valid = false;
              break;
            }
            loadedCandidates[index] = loadedCandidate;
          }
          if (valid && payloadHash != header.payloadFnv1a) {
            LOG_ERR(TAG, "Rejected wlscan payload checksum: %s", paths.finalPath);
            valid = false;
          }
        }
      }
    }
  }

  if (!closeFile(file, paths.finalPath)) valid = false;
  if (!valid) return false;

  candidates_ = std::move(loadedCandidates);
  candidateCount_ = header.candidateCount;
  cursor_ = candidateCount_ > 0 && header.cursor < candidateCount_ ? header.cursor : 0;
  return true;
}

void PageWordScanCache::clear() {
  candidates_.reset();
  candidateCount_ = 0;
  cursor_ = 0;
}

const PageWordCandidate* PageWordScanCache::candidate(const uint16_t index) const {
  return candidates_ && index < candidateCount_ ? &candidates_[index] : nullptr;
}
