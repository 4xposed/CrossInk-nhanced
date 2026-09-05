#include "RubyGlossary.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace RubyGlossary {
namespace {
constexpr char TAG[] = "RUBY";
constexpr char FILE_SUFFIX[] = "/ruby.bin";
constexpr char TMP_SUFFIX[] = ".tmp";
constexpr char BACKUP_SUFFIX[] = ".bak";
constexpr size_t MAX_BOOK_CACHE_PATH_BYTES = 512;
constexpr size_t COLLECT_HEAP_HEADROOM = 8 * 1024;
constexpr size_t ALLOCATION_OVERHEAD = 64;

struct GlossaryPaths {
  std::unique_ptr<char[]> storage;
  char* finalPath = nullptr;
  char* tmpPath = nullptr;
  char* backupPath = nullptr;
};

struct LoadedGlossary {
  std::unique_ptr<uint8_t[]> bytes;
  size_t size = 0;
  uint16_t count = 0;
};

struct RecordView {
  std::string_view base;
  std::string_view ruby;
  size_t offset = 0;
  size_t bytes = 0;
};

enum class LoadStatus : uint8_t { Valid, Missing, Corrupt, IoError, OutOfMemory };

bool decodeUtf8(const std::string_view text, size_t& offset, uint32_t& codepoint) {
  if (offset >= text.size()) return false;
  const auto first = static_cast<uint8_t>(text[offset]);
  size_t length = 0;
  uint32_t minimum = 0;
  if (first < 0x80) {
    codepoint = first;
    ++offset;
    return true;
  }
  if (first >= 0xC2 && first <= 0xDF) {
    length = 2;
    minimum = 0x80;
    codepoint = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    length = 3;
    minimum = 0x800;
    codepoint = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    length = 4;
    minimum = 0x10000;
    codepoint = first & 0x07;
  } else {
    return false;
  }
  if (length > text.size() - offset) return false;
  for (size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<uint8_t>(text[offset + index]);
    if ((continuation & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
  offset += length;
  return true;
}

bool validUtf8(const std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t codepoint = 0;
    if (!decodeUtf8(text, offset, codepoint)) return false;
  }
  return true;
}

bool hasCjkIdeograph(const std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t codepoint = 0;
    if (!decodeUtf8(text, offset, codepoint)) return false;
    if ((codepoint >= 0x3400 && codepoint <= 0x4DBF) || (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) || (codepoint >= 0x20000 && codepoint <= 0x2FA1F)) {
      return true;
    }
  }
  return false;
}

bool validPair(const std::string_view base, const std::string_view ruby) {
  return !base.empty() && !ruby.empty() && base != ruby && base.size() <= kMaxTextBytes &&
         ruby.size() <= kMaxTextBytes && validUtf8(base) && validUtf8(ruby) && hasCjkIdeograph(base);
}

bool safeCollectAllocation(const std::vector<Pair>& pairs, const size_t baseBytes, const size_t rubyBytes,
                           size_t& nextCapacity) {
  nextCapacity = pairs.capacity();
  size_t vectorBytes = 0;
  if (pairs.size() == pairs.capacity()) {
    nextCapacity = pairs.empty() ? 32 : std::min(kMaxPairsPerSection, pairs.capacity() * 2);
    if (nextCapacity <= pairs.capacity() || nextCapacity > std::numeric_limits<size_t>::max() / sizeof(Pair)) {
      return false;
    }
    vectorBytes = nextCapacity * sizeof(Pair);
  }
  const size_t firstCopy = baseBytes + 1;
  const size_t secondCopy = rubyBytes + 1;
  const size_t largest = std::max({vectorBytes, firstCopy, secondCopy});
  if (largest > std::numeric_limits<size_t>::max() - COLLECT_HEAP_HEADROOM - ALLOCATION_OVERHEAD) return false;
  if (vectorBytes >
      std::numeric_limits<size_t>::max() - firstCopy - secondCopy - COLLECT_HEAP_HEADROOM - ALLOCATION_OVERHEAD) {
    return false;
  }
  const size_t total = vectorBytes + firstCopy + secondCopy + COLLECT_HEAP_HEADROOM + ALLOCATION_OVERHEAD;
  return ESP.getMaxAllocHeap() >= largest + COLLECT_HEAP_HEADROOM + ALLOCATION_OVERHEAD && ESP.getFreeHeap() >= total;
}

bool appendElementText(std::string& destination, const std::string_view text) {
  if (destination.size() > kMaxTextBytes || text.size() > kMaxTextBytes - destination.size()) return false;
  const size_t required = destination.size() + text.size();
  if (required > destination.capacity()) {
    const size_t request = kMaxTextBytes + 1;
    if (ESP.getMaxAllocHeap() < request + COLLECT_HEAP_HEADROOM ||
        ESP.getFreeHeap() < request + COLLECT_HEAP_HEADROOM) {
      return false;
    }
    destination.reserve(kMaxTextBytes);
  }
  destination.append(text.data(), text.size());
  return true;
}

bool makePaths(const std::string_view bookCachePath, GlossaryPaths& paths, bool& outOfMemory) {
  outOfMemory = false;
  if (bookCachePath.empty() || bookCachePath.size() > MAX_BOOK_CACHE_PATH_BYTES ||
      bookCachePath.find('\0') != std::string_view::npos) {
    LOG_ERR(TAG, "Invalid book cache path");
    return false;
  }
  constexpr size_t fileSuffixBytes = sizeof(FILE_SUFFIX) - 1;
  constexpr size_t siblingSuffixBytes = sizeof(TMP_SUFFIX) - 1;
  if (bookCachePath.size() > std::numeric_limits<size_t>::max() - fileSuffixBytes - siblingSuffixBytes - 1) {
    LOG_ERR(TAG, "Glossary path size overflow");
    return false;
  }
  const size_t finalLength = bookCachePath.size() + fileSuffixBytes;
  const size_t finalBytes = finalLength + 1;
  const size_t siblingBytes = finalLength + siblingSuffixBytes + 1;
  if (siblingBytes > (std::numeric_limits<size_t>::max() - finalBytes) / 2) {
    LOG_ERR(TAG, "Glossary sibling path size overflow");
    return false;
  }
  const size_t allocationBytes = finalBytes + siblingBytes * 2;
  // This single fallible allocation is bounded to 1,578 bytes or we would exceed the firmware's small task-frame budget.
  paths.storage = makeUniqueNoThrow<char[]>(allocationBytes);
  if (!paths.storage) {
    LOG_ERR(TAG, "OOM allocating %u-byte glossary path scratch", static_cast<unsigned>(allocationBytes));
    outOfMemory = true;
    return false;
  }
  paths.finalPath = paths.storage.get();
  paths.tmpPath = paths.finalPath + finalBytes;
  paths.backupPath = paths.tmpPath + siblingBytes;
  std::memcpy(paths.finalPath, bookCachePath.data(), bookCachePath.size());
  std::memcpy(paths.finalPath + bookCachePath.size(), FILE_SUFFIX, sizeof(FILE_SUFFIX));
  std::memcpy(paths.tmpPath, paths.finalPath, finalLength);
  std::memcpy(paths.tmpPath + finalLength, TMP_SUFFIX, sizeof(TMP_SUFFIX));
  std::memcpy(paths.backupPath, paths.finalPath, finalLength);
  std::memcpy(paths.backupPath + finalLength, BACKUP_SUFFIX, sizeof(BACKUP_SUFFIX));
  return true;
}

bool closeFile(HalFile& file, const char* path) {
  (void)path;  // Test logging macros compile arguments out.
  if (!file.isOpen()) return true;
  if (file.close()) return true;
  LOG_ERR(TAG, "Failed to close glossary file: %s", path);
  return false;
}

bool removeExisting(const char* path) {
  if (!Storage.exists(path)) return true;
  if (Storage.remove(path)) return true;
  LOG_ERR(TAG, "Failed to remove glossary file: %s", path);
  return false;
}

bool recoverBackup(const GlossaryPaths& paths, const bool requireStaleRemoval) {
  if (!Storage.exists(paths.backupPath)) return true;
  if (Storage.exists(paths.finalPath)) {
    if (Storage.remove(paths.backupPath)) return true;
    LOG_ERR(TAG, "Failed to remove stale glossary backup: %s", paths.backupPath);
    return !requireStaleRemoval;
  }
  if (Storage.rename(paths.backupPath, paths.finalPath)) {
    LOG_INF(TAG, "Recovered glossary backup: %s", paths.finalPath);
    return true;
  }
  LOG_ERR(TAG, "Failed to recover glossary backup: %s", paths.finalPath);
  return false;
}

bool readExact(HalFile& file, void* destination, const size_t count, const char* path) {
  (void)path;  // Test logging macros compile arguments out.
  const int bytesRead = file.read(destination, count);
  if (bytesRead == static_cast<int>(count)) return true;
  LOG_ERR(TAG, "Short glossary read from %s: wanted %u, got %d", path, static_cast<unsigned>(count), bytesRead);
  return false;
}

bool writeExact(HalFile& file, const void* source, const size_t count, const char* path) {
  (void)path;  // Test logging macros compile arguments out.
  const size_t bytesWritten = file.write(source, count);
  if (bytesWritten == count) return true;
  LOG_ERR(TAG, "Short glossary write to %s: wanted %u, got %u", path, static_cast<unsigned>(count),
          static_cast<unsigned>(bytesWritten));
  return false;
}

bool nextRecord(const uint8_t* bytes, const size_t size, size_t& offset, RecordView& record) {
  if (offset >= size) return false;
  const size_t recordOffset = offset;
  const uint8_t baseLength = bytes[offset++];
  if (baseLength == 0 || baseLength > kMaxTextBytes || baseLength > size - offset) return false;
  const std::string_view base(reinterpret_cast<const char*>(bytes + offset), baseLength);
  offset += baseLength;
  if (offset >= size) return false;
  const uint8_t rubyLength = bytes[offset++];
  if (rubyLength == 0 || rubyLength > kMaxTextBytes || rubyLength > size - offset) return false;
  const std::string_view ruby(reinterpret_cast<const char*>(bytes + offset), rubyLength);
  offset += rubyLength;
  if (!validUtf8(base) || !validUtf8(ruby)) return false;
  record = {base, ruby, recordOffset, offset - recordOffset};
  return true;
}

bool validateBuffer(const uint8_t* bytes, const size_t size, uint16_t& count) {
  if (!bytes || size < 3 || size > kMaxFileBytes || bytes[0] != kFileVersion) return false;
  count = static_cast<uint16_t>(bytes[1]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[2]) << 8);
  if (count > kMaxFileRecords) return false;
  size_t offset = 3;
  for (uint16_t index = 0; index < count; ++index) {
    RecordView record;
    if (!nextRecord(bytes, size, offset, record)) return false;
  }
  return offset == size;
}

LoadStatus loadValidated(const char* path, LoadedGlossary& loaded) {
  loaded = {};
  if (!Storage.exists(path)) return LoadStatus::Missing;
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "Failed to open glossary for reading: %s", path);
    return LoadStatus::IoError;
  }
  const size_t size = file.fileSize();
  if (size < 3 || size > kMaxFileBytes) {
    LOG_ERR(TAG, "Rejected glossary size %u: %s", static_cast<unsigned>(size), path);
    return closeFile(file, path) ? LoadStatus::Corrupt : LoadStatus::IoError;
  }
  // Whole-file validation needs random bounded duplicate scans.
	// Runtime file size is unsuitable for the small task stack.
	// this exact fallible buffer is capped at 16 KiB and released before temp-file validation/promotion.
  auto bytes = makeUniqueNoThrow<uint8_t[]>(size);
  if (!bytes) {
    LOG_ERR(TAG, "OOM allocating %u-byte glossary read buffer", static_cast<unsigned>(size));
    closeFile(file, path);
    return LoadStatus::OutOfMemory;
  }
  const bool read = readExact(file, bytes.get(), size, path);
  const bool closed = closeFile(file, path);
  if (!read || !closed) return LoadStatus::IoError;
  uint16_t count = 0;
  if (!validateBuffer(bytes.get(), size, count)) {
    LOG_ERR(TAG, "Rejected malformed glossary: %s", path);
    return LoadStatus::Corrupt;
  }
  loaded.bytes = std::move(bytes);
  loaded.size = size;
  loaded.count = count;
  return LoadStatus::Valid;
}

bool seenEarlier(const uint8_t* bytes, const size_t currentOffset, const RecordView& wanted,
                 const bool requireBaseOnly) {
  size_t offset = 3;
  while (offset < currentOffset) {
    RecordView prior;
    if (!nextRecord(bytes, currentOffset, offset, prior)) return false;
    if (prior.base == wanted.base && (requireBaseOnly || prior.ruby == wanted.ruby)) return true;
  }
  return false;
}

bool oldContains(const LoadedGlossary& old, const std::string_view base, const std::string_view ruby) {
  size_t offset = 3;
  for (uint16_t index = 0; index < old.count; ++index) {
    RecordView record;
    if (!nextRecord(old.bytes.get(), old.size, offset, record)) return false;
    if (record.base == base && record.ruby == ruby) return true;
  }
  return false;
}

bool acceptedNewContains(const std::vector<Pair>& pairs, const std::array<uint8_t, 25>& accepted,
                         const size_t beforeIndex, const Pair& wanted) {
  for (size_t index = 0; index < beforeIndex; ++index) {
    if ((accepted[index / 8] & static_cast<uint8_t>(1U << (index % 8))) != 0 && pairs[index] == wanted) return true;
  }
  return false;
}

bool writeRecord(HalFile& file, const std::string_view base, const std::string_view ruby, const char* path) {
  const uint8_t baseLength = static_cast<uint8_t>(base.size());
  const uint8_t rubyLength = static_cast<uint8_t>(ruby.size());
  return writeExact(file, &baseLength, 1, path) && writeExact(file, base.data(), base.size(), path) &&
         writeExact(file, &rubyLength, 1, path) && writeExact(file, ruby.data(), ruby.size(), path);
}

bool promoteTemp(const GlossaryPaths& paths) {
  if (!Storage.exists(paths.finalPath)) {
    if (Storage.rename(paths.tmpPath, paths.finalPath)) return true;
    LOG_ERR(TAG, "Failed to promote first glossary: %s", paths.finalPath);
    removeExisting(paths.tmpPath);
    return false;
  }
  if (!Storage.rename(paths.finalPath, paths.backupPath)) {
    LOG_ERR(TAG, "Failed to preserve old glossary before replacement");
    removeExisting(paths.tmpPath);
    return false;
  }
  if (Storage.rename(paths.tmpPath, paths.finalPath)) {
    if (!Storage.remove(paths.backupPath)) {
      LOG_ERR(TAG, "Failed to remove promoted glossary backup: %s", paths.backupPath);
    }
    return true;
  }
  LOG_ERR(TAG, "Failed to promote glossary; restoring previous file");
  if (!Storage.rename(paths.backupPath, paths.finalPath)) {
    LOG_ERR(TAG, "Failed to restore glossary backup: %s", paths.finalPath);
  }
  removeExisting(paths.tmpPath);
  return false;
}
}  // namespace

void collectView(std::vector<Pair>& pairs, const std::string_view base, const std::string_view ruby) {
  if (pairs.size() >= kMaxPairsPerSection || !validPair(base, ruby)) return;
  if (std::any_of(pairs.begin(), pairs.end(), [base, ruby](const Pair& pair) {
        return std::string_view(pair.first) == base && std::string_view(pair.second) == ruby;
      })) {
    return;
  }
  size_t nextCapacity = pairs.capacity();
  if (!safeCollectAllocation(pairs, base.size(), ruby.size(), nextCapacity)) return;
  if (nextCapacity != pairs.capacity()) pairs.reserve(nextCapacity);
  pairs.emplace_back(std::string(base), std::string(ruby));
}

void collect(std::vector<Pair>& pairs, const std::string& base, const std::string& ruby) {
  collectView(pairs, base, ruby);
}

void resetElement(std::string& elementBase, std::string& elementRuby, int& runCount) {
  elementBase.clear();
  elementRuby.clear();
  runCount = 0;
}

void resetHarvest(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount) {
  pairs.clear();
  resetElement(elementBase, elementRuby, runCount);
}

void collectRun(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount,
                const std::string_view base, const std::string_view ruby) {
  collectView(pairs, base, ruby);
  // A kana-only or identical individual run is not useful by itself, but it is still part of a
	// surrounding kanji-bearing compound (for example 食べ).
  if (runCount < 0 || base.empty() || ruby.empty() || !validUtf8(base) || !validUtf8(ruby) ||
      !appendElementText(elementBase, base) || !appendElementText(elementRuby, ruby)) {
    elementBase.clear();
    elementRuby.clear();
    runCount = -1;
    return;
  }
  ++runCount;
}

void finishElement(std::vector<Pair>& pairs, std::string& elementBase, std::string& elementRuby, int& runCount) {
  if (runCount >= 2) collectView(pairs, elementBase, elementRuby);
  resetElement(elementBase, elementRuby, runCount);
}

void merge(const std::string& bookCachePath, const std::vector<Pair>& pairs) {
  if (pairs.empty() || bookCachePath.empty()) return;
  GlossaryPaths paths;
  bool pathOom = false;
  if (!makePaths(bookCachePath, paths, pathOom)) return;
  if (!recoverBackup(paths, true) || !removeExisting(paths.tmpPath)) return;

  LoadedGlossary old;
  const LoadStatus oldStatus = loadValidated(paths.finalPath, old);
  if (oldStatus == LoadStatus::OutOfMemory || oldStatus == LoadStatus::IoError) return;
  const bool validOld = oldStatus == LoadStatus::Valid;

  uint16_t uniqueOldCount = 0;
  size_t uniqueOldBytes = 0;
  bool oldHadDuplicates = false;
  if (validOld) {
    size_t offset = 3;
    for (uint16_t index = 0; index < old.count; ++index) {
      RecordView record;
      nextRecord(old.bytes.get(), old.size, offset, record);
      if (seenEarlier(old.bytes.get(), record.offset, record, false)) {
        oldHadDuplicates = true;
      } else {
        ++uniqueOldCount;
        uniqueOldBytes += record.bytes;
      }
    }
  }

  std::array<uint8_t, 25> accepted{};  // One bit per bounded section-harvest pair (200 bits / 25 bytes).
  uint16_t acceptedCount = 0;
  size_t acceptedBytes = 0;
  const size_t inspectCount = std::min(pairs.size(), kMaxPairsPerSection);
  for (size_t index = 0; index < inspectCount; ++index) {
    const Pair& pair = pairs[index];
    if (!validPair(pair.first, pair.second) || (validOld && oldContains(old, pair.first, pair.second)) ||
        acceptedNewContains(pairs, accepted, index, pair)) {
      continue;
    }
    const size_t recordBytes = 2 + pair.first.size() + pair.second.size();
    if (uniqueOldCount + acceptedCount >= kMaxFileRecords ||
        recordBytes > kMaxFileBytes - 3 - uniqueOldBytes - acceptedBytes) {
      continue;
    }
    accepted[index / 8] |= static_cast<uint8_t>(1U << (index % 8));
    ++acceptedCount;
    acceptedBytes += recordBytes;
  }
  if (acceptedCount == 0 && (!validOld || !oldHadDuplicates)) return;

  const uint16_t outputCount = static_cast<uint16_t>(uniqueOldCount + acceptedCount);
  HalFile file;
  if (!Storage.openFileForWrite(TAG, paths.tmpPath, file)) {
    LOG_ERR(TAG, "Failed to open glossary temp for writing: %s", paths.tmpPath);
    removeExisting(paths.tmpPath);
    return;
  }
  const uint8_t header[3] = {kFileVersion, static_cast<uint8_t>(outputCount), static_cast<uint8_t>(outputCount >> 8)};
  bool wrote = writeExact(file, header, sizeof(header), paths.tmpPath);
  if (validOld) {
    size_t offset = 3;
    for (uint16_t index = 0; wrote && index < old.count; ++index) {
      RecordView record;
      nextRecord(old.bytes.get(), old.size, offset, record);
      if (!seenEarlier(old.bytes.get(), record.offset, record, false)) {
        wrote = writeRecord(file, record.base, record.ruby, paths.tmpPath);
      }
    }
  }
  for (size_t index = 0; wrote && index < inspectCount; ++index) {
    if ((accepted[index / 8] & static_cast<uint8_t>(1U << (index % 8))) != 0) {
      wrote = writeRecord(file, pairs[index].first, pairs[index].second, paths.tmpPath);
    }
  }
  if (wrote && !file.sync()) {
    LOG_ERR(TAG, "Failed to sync glossary temp: %s", paths.tmpPath);
    wrote = false;
  }
  if (!closeFile(file, paths.tmpPath)) wrote = false;
  old.bytes.reset();
  if (!wrote) {
    removeExisting(paths.tmpPath);
    return;
  }

  LoadedGlossary verified;
  const LoadStatus verifiedStatus = loadValidated(paths.tmpPath, verified);
  if (verifiedStatus != LoadStatus::Valid || verified.count != outputCount) {
    LOG_ERR(TAG, "Failed to validate completed glossary temp: %s", paths.tmpPath);
    verified.bytes.reset();
    removeExisting(paths.tmpPath);
    return;
  }
  verified.bytes.reset();
  if (promoteTemp(paths)) {
    LOG_DBG(TAG, "Glossary merged: +%u pairs (%u total)", static_cast<unsigned>(acceptedCount),
            static_cast<unsigned>(outputCount));
  }
}

LookupStatus lookupOwned(const std::string_view bookCachePath, const std::string_view base,
                         OwnedReadings& outReadings) {
  outReadings.reset();
  if (base.empty() || base.size() > kMaxTextBytes || !validUtf8(base)) return LookupStatus::NotFound;
  GlossaryPaths paths;
  bool pathOom = false;
  if (!makePaths(bookCachePath, paths, pathOom)) {
    return pathOom ? LookupStatus::OutOfMemory : LookupStatus::NotFound;
  }
  if (!recoverBackup(paths, false)) return LookupStatus::NotFound;
  LoadedGlossary loaded;
  const LoadStatus loadStatus = loadValidated(paths.finalPath, loaded);
  if (loadStatus == LoadStatus::OutOfMemory) return LookupStatus::OutOfMemory;
  if (loadStatus != LoadStatus::Valid) return LookupStatus::NotFound;

  size_t outputBytes = 0;
  uint16_t readingCount = 0;
  size_t offset = 3;
  for (uint16_t index = 0; index < loaded.count; ++index) {
    RecordView record;
    nextRecord(loaded.bytes.get(), loaded.size, offset, record);
    if (record.base == base && !seenEarlier(loaded.bytes.get(), record.offset, record, false)) {
      if (readingCount > 0) outputBytes += 3;  // UTF-8 middle dot: U+30FB.
      outputBytes += record.ruby.size();
      ++readingCount;
    }
  }
  if (readingCount == 0) return LookupStatus::NotFound;

  // The joined output can approach the complete 16 KiB file cap, so it cannot live on the task stack.
	// Allocate it exactly once and transfer ownership to the backend only after every byte has been filled.
  auto output = makeUniqueNoThrow<char[]>(outputBytes + 1);
  if (!output) {
    LOG_ERR(TAG, "OOM allocating %u-byte glossary result", static_cast<unsigned>(outputBytes + 1));
    return LookupStatus::OutOfMemory;
  }
  size_t written = 0;
  offset = 3;
  for (uint16_t index = 0; index < loaded.count; ++index) {
    RecordView record;
    nextRecord(loaded.bytes.get(), loaded.size, offset, record);
    if (record.base != base || seenEarlier(loaded.bytes.get(), record.offset, record, false)) continue;
    if (written > 0) {
      constexpr uint8_t middleDot[] = {0xE3, 0x83, 0xBB};
      std::memcpy(output.get() + written, middleDot, sizeof(middleDot));
      written += sizeof(middleDot);
    }
    std::memcpy(output.get() + written, record.ruby.data(), record.ruby.size());
    written += record.ruby.size();
  }
  output[written] = '\0';
  outReadings.storage_ = std::move(output);
  outReadings.length_ = written;
  return LookupStatus::Found;
}

bool lookup(const std::string& bookCachePath, const std::string& base, std::string& outReadings) {
  outReadings.clear();
  OwnedReadings owned;
  if (lookupOwned(bookCachePath, base, owned) != LookupStatus::Found) return false;
  const std::string_view readings = owned.view();
  outReadings.assign(readings.data(), readings.size());
  return true;
}
}  // namespace RubyGlossary
