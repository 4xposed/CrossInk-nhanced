#include "DictIndex.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace {
constexpr uint8_t SPX_MAGIC[8] = {'C', 'P', 'S', 'P', 'X', '1', 0, 0};
constexpr uint32_t SPX_VERSION = 1;
constexpr uint32_t SPX_STRIDE = 48;
constexpr size_t SPX_HEADER_SIZE = 32;
constexpr size_t SPX_KEY_SIZE = DictIndexRecord::HEADWORD_SIZE;
constexpr size_t MAX_COARSE = 128;
constexpr size_t FINE_SLICE_BYTES = 3904;
constexpr size_t BLOCK_RECORDS = 64;
constexpr size_t MAX_SIBLINGS = 32;
constexpr size_t MAX_RUN_SCAN = 512;
constexpr size_t MAX_MERGED_ENTRIES = 5;
constexpr size_t MAX_DEFINITION_BYTES = 16 * 1024;
constexpr size_t DEFINITION_HEAP_HEADROOM = 8 * 1024;
constexpr char MERGE_SEPARATOR[] = "\n\n---\n";

struct PathPair {
  const char* idx;
  const char* dat;
};

constexpr PathPair VOCAB_PATHS[] = {
    {"/dictionaries/jp/vocab.idx", "/dictionaries/jp/vocab.dat"},
    {"/dictionaries/jp/jmdict.idx", "/dictionaries/jp/jmdict.dat"},
    {"/dict/vocab.idx", "/dict/vocab.dat"},
    {"/dict/jmdict.idx", "/dict/jmdict.dat"},
};
constexpr PathPair GRAMMAR_PATHS[] = {
    {"/dictionaries/jp/grammar.idx", "/dictionaries/jp/grammar.dat"},
    {"/dict/grammar.idx", "/dict/grammar.dat"},
};
constexpr PathPair NAMES_PATHS[] = {
    {"/dictionaries/jp/names.idx", "/dictionaries/jp/names.dat"},
    {"/dictionaries/jp/jmnedict.idx", "/dictionaries/jp/jmnedict.dat"},
    {"/dict/names.idx", "/dict/names.dat"},
    {"/dict/jmnedict.idx", "/dict/jmnedict.dat"},
};

struct CoarseEntry {
  char key[SPX_KEY_SIZE];
  uint32_t fineIndex;
};

struct SourceState {
  CooperativeCancellation cancellation{};
  HalFile idxFile;
  HalFile datFile;
  HalFile spxFile;
  const char* idxPath = nullptr;
  const char* datPath = nullptr;
  size_t idxSize = 0;
  size_t datSize = 0;
  size_t recordCount = 0;
  uint8_t source = 0;
  bool available = false;

  // These exact-sized, session-lifetime allocations avoid both large task stack
  // frames and repeated heap churn during the thousands of probes in a page scan.
  std::unique_ptr<uint8_t[]> blockCache;
  bool blockCacheTried = false;
  size_t blockStart = SIZE_MAX;
  size_t blockCount = 0;

  bool spxTried = false;
  bool spxOk = false;
  uint32_t spxFineCount = 0;
  uint32_t spxCoarseStride = 0;
  uint32_t spxStride = 0;
  size_t spxCoarseCount = 0;
  std::unique_ptr<CoarseEntry[]> coarse;
  std::unique_ptr<uint8_t[]> fineCache;
  size_t fineCacheBytes = 0;
  uint32_t fineCacheFirst = UINT32_MAX;
  size_t fineCacheCount = 0;

  void close() {
    if (spxFile.isOpen()) spxFile.close();
    if (datFile.isOpen()) datFile.close();
    if (idxFile.isOpen()) idxFile.close();
    blockCache.reset();
    coarse.reset();
    fineCache.reset();
    idxPath = nullptr;
    datPath = nullptr;
    idxSize = 0;
    datSize = 0;
    recordCount = 0;
    source = 0;
    available = false;
    blockCacheTried = false;
    blockStart = SIZE_MAX;
    blockCount = 0;
    spxTried = false;
    spxOk = false;
    spxFineCount = 0;
    spxCoarseStride = 0;
    spxStride = 0;
    spxCoarseCount = 0;
    fineCacheBytes = 0;
    fineCacheFirst = UINT32_MAX;
    fineCacheCount = 0;
  }
};

struct RankedSibling {
  uint32_t offset;
  uint16_t length;
  uint8_t priority;
  uint8_t posFlags;
};
static_assert(sizeof(RankedSibling) == 8);

bool readExact(HalFile& file, size_t offset, void* destination, size_t length,
               CooperativeCancellation cancellation = {}) {
  if (cancellation.requested()) return false;
  if (!file.seek(offset)) {
    LOG_ERR("DICT", "Dictionary seek failed at %u", static_cast<unsigned>(offset));
    return false;
  }
  auto* output = static_cast<uint8_t*>(destination);
  size_t completed = 0;
  while (completed < length) {
    if (cancellation.requested()) return false;
    // Check cancellation between bounded SD reads, including long senses.
    // Keep the existing single-read contract for callers without cancellation.
    const size_t chunk = cancellation.callback ? std::min<size_t>(512, length - completed) : length - completed;
    const int read = file.read(output + completed, chunk);
    if (cancellation.requested()) return false;
    if (read != static_cast<int>(chunk)) {
      LOG_ERR("DICT", "Dictionary short read at %u: %d/%u", static_cast<unsigned>(offset + completed), read,
              static_cast<unsigned>(chunk));
      return false;
    }
    completed += chunk;
  }
  return !cancellation.requested();
}

JapaneseDictStatus readFailureStatus(const SourceState& source) {
  return source.cancellation.requested() ? JapaneseDictStatus::Cancelled : JapaneseDictStatus::ReadError;
}

bool validUtf8(const char* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const uint8_t first = static_cast<uint8_t>(data[offset++]);
    if (first <= 0x7f) continue;
    uint32_t codepoint = 0;
    size_t continuation = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      continuation = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      continuation = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      continuation = 3;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (continuation > length - offset) return false;
    for (size_t index = 0; index < continuation; ++index) {
      const uint8_t next = static_cast<uint8_t>(data[offset++]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

bool decodeRecord(const uint8_t* bytes, size_t datSize, DictIndexRecord& out) {
  std::memcpy(out.headword, bytes, DictIndexRecord::HEADWORD_SIZE);
  std::memcpy(&out.offset, bytes + 32, sizeof(out.offset));
  std::memcpy(&out.length, bytes + 36, sizeof(out.length));
  out.priority = bytes[38];
  out.posFlags = bytes[39];

  const void* nul = std::memchr(out.headword, 0, DictIndexRecord::HEADWORD_SIZE);
  if (!nul) return false;
  const size_t headwordLength = static_cast<const char*>(nul) - out.headword;
  if (headwordLength == 0) return false;
  for (size_t index = headwordLength; index < DictIndexRecord::HEADWORD_SIZE; ++index) {
    if (out.headword[index] != 0) return false;
  }
  if (!validUtf8(out.headword, headwordLength)) return false;
  return out.offset <= datSize && out.length <= datSize - out.offset;
}

JapaneseDictStatus readRecord(SourceState& source, size_t recordIndex, DictIndexRecord& out) {
  if (source.cancellation.requested()) return JapaneseDictStatus::Cancelled;
  if (recordIndex >= source.recordCount) return readFailureStatus(source);

  std::array<uint8_t, sizeof(DictIndexRecord)> direct{};
  const uint8_t* recordBytes = direct.data();
  if (source.blockCache) {
    if (source.blockStart == SIZE_MAX || recordIndex < source.blockStart ||
        recordIndex >= source.blockStart + source.blockCount) {
      const size_t halfBlock = BLOCK_RECORDS / 2;
      const size_t start = recordIndex > halfBlock ? recordIndex - halfBlock : 0;
      const size_t count = std::min(BLOCK_RECORDS, source.recordCount - start);
      const size_t bytes = count * sizeof(DictIndexRecord);
      if (!readExact(source.idxFile, start * sizeof(DictIndexRecord), source.blockCache.get(), bytes,
                     source.cancellation)) {
        source.blockStart = SIZE_MAX;
        source.blockCount = 0;
        return readFailureStatus(source);
      }
      source.blockStart = start;
      source.blockCount = count;
    }
    recordBytes = source.blockCache.get() + (recordIndex - source.blockStart) * sizeof(DictIndexRecord);
  } else if (!readExact(source.idxFile, recordIndex * sizeof(DictIndexRecord), direct.data(), direct.size(),
                        source.cancellation)) {
    return readFailureStatus(source);
  }

  if (!decodeRecord(recordBytes, source.datSize, out)) {
    LOG_ERR("DICT", "Malformed dictionary record %u", static_cast<unsigned>(recordIndex));
    return readFailureStatus(source);
  }
  return JapaneseDictStatus::Found;
}

template <size_t Count>
const PathPair* resolvePath(const PathPair (&paths)[Count]) {
  for (const auto& path : paths) {
    if (Storage.exists(path.idx)) return &path;
  }
  return nullptr;
}

JapaneseDictStatus openSource(SourceState& source, const PathPair& path, uint8_t sourceId) {
  if (source.cancellation.requested()) return JapaneseDictStatus::Cancelled;
  source.idxPath = path.idx;
  source.datPath = path.dat;
  source.source = sourceId;
  if (!Storage.exists(path.dat)) {
    LOG_ERR("DICT", "Dictionary index %s has no matching data file", path.idx);
    return JapaneseDictStatus::Unavailable;
  }
  if (!Storage.openFileForRead("DICT", path.idx, source.idxFile)) {
    LOG_ERR("DICT", "Could not open dictionary index %s", path.idx);
    return JapaneseDictStatus::ReadError;
  }
  if (!Storage.openFileForRead("DICT", path.dat, source.datFile)) {
    LOG_ERR("DICT", "Could not open dictionary data %s", path.dat);
    if (source.idxFile.isOpen()) source.idxFile.close();
    return JapaneseDictStatus::ReadError;
  }
  source.idxSize = source.idxFile.size();
  source.datSize = source.datFile.size();
  if (source.idxSize % sizeof(DictIndexRecord) != 0) {
    LOG_ERR("DICT", "Index %s has a partial record", path.idx);
    return JapaneseDictStatus::ReadError;
  }
  source.recordCount = source.idxSize / sizeof(DictIndexRecord);
  source.available = true;
  return JapaneseDictStatus::Found;
}

void loadSparseIndex(SourceState& source) {
  if (source.cancellation.requested()) return;
  source.spxTried = true;
  char spxPath[64]{};
  const size_t pathLength = std::strlen(source.idxPath);
  if (pathLength < 4 || pathLength >= sizeof(spxPath) || std::memcmp(source.idxPath + pathLength - 4, ".idx", 4) != 0) {
    LOG_ERR("DICT", "Cannot derive sparse-index path from %s", source.idxPath);
    return;
  }
  std::memcpy(spxPath, source.idxPath, pathLength + 1);
  std::memcpy(spxPath + pathLength - 4, ".spx", 4);
  if (!Storage.exists(spxPath)) return;
  if (!Storage.openFileForRead("DICT", spxPath, source.spxFile)) {
    LOG_ERR("DICT", "Could not open sparse index %s", spxPath);
    return;
  }

  std::array<uint8_t, SPX_HEADER_SIZE> header{};
  if (!readExact(source.spxFile, 0, header.data(), header.size(), source.cancellation)) return;
  if (std::memcmp(header.data(), SPX_MAGIC, sizeof(SPX_MAGIC)) != 0) {
    LOG_ERR("DICT", "Sparse index %s has invalid magic", spxPath);
    return;
  }
  uint32_t version = 0;
  uint32_t stride = 0;
  uint32_t recordCount = 0;
  uint32_t fineCount = 0;
  uint32_t reserved = 0;
  std::memcpy(&version, header.data() + 8, sizeof(version));
  std::memcpy(&stride, header.data() + 12, sizeof(stride));
  std::memcpy(&recordCount, header.data() + 16, sizeof(recordCount));
  std::memcpy(&fineCount, header.data() + 20, sizeof(fineCount));
  std::memcpy(&reserved, header.data() + 24, sizeof(reserved));
  const size_t expectedFine = (source.recordCount + SPX_STRIDE - 1) / SPX_STRIDE;
  if (version != SPX_VERSION || stride != SPX_STRIDE || recordCount != source.recordCount ||
      fineCount != expectedFine || reserved != 0 || fineCount == 0 ||
      source.spxFile.size() != SPX_HEADER_SIZE + static_cast<size_t>(fineCount) * SPX_KEY_SIZE) {
    LOG_ERR("DICT", "Sparse index %s is stale or malformed; using direct search", spxPath);
    return;
  }

  const uint32_t coarseStride = (fineCount + MAX_COARSE - 1) / MAX_COARSE;
  const size_t fineBytes = static_cast<size_t>(coarseStride) * SPX_KEY_SIZE;
  const size_t coarseCount = (fineCount + coarseStride - 1) / coarseStride;
  if (fineBytes > FINE_SLICE_BYTES) {
    LOG_ERR("DICT", "Sparse index %s exceeds the bounded fine cache", spxPath);
    return;
  }

  // At most 128 * 36 = 4,608 bytes, exact-sized and session-lived. Static
  // storage would make three dictionary sessions permanently consume DRAM.
  auto coarse = makeUniqueNoThrow<CoarseEntry[]>(coarseCount);
  // At most 3,904 bytes, reused for one fine bracket instead of allocating in probes.
  auto fineCache = makeUniqueNoThrow<uint8_t[]>(fineBytes);
  if (!coarse || !fineCache) {
    LOG_ERR("DICT", "Sparse index cache allocation failed; using direct search");
    return;
  }
  for (size_t index = 0; index < coarseCount; ++index) {
    if (source.cancellation.requested()) return;
    const uint32_t fineIndex = static_cast<uint32_t>(index) * coarseStride;
    if (!readExact(source.spxFile, SPX_HEADER_SIZE + static_cast<size_t>(fineIndex) * SPX_KEY_SIZE, coarse[index].key,
                   SPX_KEY_SIZE, source.cancellation)) {
      return;
    }
    coarse[index].fineIndex = fineIndex;
  }
  source.coarse = std::move(coarse);
  source.fineCache = std::move(fineCache);
  source.fineCacheBytes = fineBytes;
  source.spxFineCount = fineCount;
  source.spxCoarseStride = coarseStride;
  source.spxStride = stride;
  source.spxCoarseCount = coarseCount;
  source.spxOk = true;
}

void prepareAccelerators(SourceState& source) {
  source.blockCacheTried = true;
  // 64 * 40 = 2,560 bytes, allocated once while opening the session so every
  // later probe remains allocation-free. Failure keeps direct record reads.
  source.blockCache = makeUniqueNoThrow<uint8_t[]>(BLOCK_RECORDS * sizeof(DictIndexRecord));
  if (!source.blockCache) LOG_ERR("DICT", "Index block cache allocation failed; using direct reads");
  loadSparseIndex(source);
}

bool narrowWithSparseIndex(SourceState& source, const char* key, size_t& lo, size_t& hi) {
  size_t coarseLo = 0;
  size_t coarseHi = source.spxCoarseCount;
  while (coarseLo < coarseHi) {
    const size_t middle = coarseLo + (coarseHi - coarseLo) / 2;
    if (std::memcmp(source.coarse[middle].key, key, SPX_KEY_SIZE) <= 0) {
      coarseLo = middle + 1;
    } else {
      coarseHi = middle;
    }
  }
  const size_t coarseIndex = coarseLo > 0 ? coarseLo - 1 : 0;
  const uint32_t fineFirst = source.coarse[coarseIndex].fineIndex;
  const uint32_t fineEnd =
      coarseIndex + 1 < source.spxCoarseCount ? source.coarse[coarseIndex + 1].fineIndex : source.spxFineCount;
  const size_t count = fineEnd - fineFirst;
  if (count == 0 || count * SPX_KEY_SIZE > source.fineCacheBytes) return false;
  if (source.fineCacheFirst != fineFirst || source.fineCacheCount != count) {
    if (!readExact(source.spxFile, SPX_HEADER_SIZE + static_cast<size_t>(fineFirst) * SPX_KEY_SIZE,
                   source.fineCache.get(), count * SPX_KEY_SIZE, source.cancellation)) {
      source.fineCacheFirst = UINT32_MAX;
      source.fineCacheCount = 0;
      return false;
    }
    source.fineCacheFirst = fineFirst;
    source.fineCacheCount = count;
  }
  size_t fineLo = 0;
  size_t fineHi = count;
  while (fineLo < fineHi) {
    const size_t middle = fineLo + (fineHi - fineLo) / 2;
    if (std::memcmp(source.fineCache.get() + middle * SPX_KEY_SIZE, key, SPX_KEY_SIZE) <= 0) {
      fineLo = middle + 1;
    } else {
      fineHi = middle;
    }
  }
  const size_t fineIndex = fineFirst + (fineLo > 0 ? fineLo - 1 : 0);
  lo = fineIndex * source.spxStride;
  hi = std::min(source.recordCount, (fineIndex + 1) * static_cast<size_t>(source.spxStride));
  return true;
}

JapaneseDictStatus binarySearch(SourceState& source, const char* key, size_t lo, size_t hi, size_t& match) {
  while (lo < hi) {
    const size_t middle = lo + (hi - lo) / 2;
    DictIndexRecord record{};
    const JapaneseDictStatus status = readRecord(source, middle, record);
    if (status != JapaneseDictStatus::Found) return status;
    const int comparison = std::memcmp(key, record.headword, SPX_KEY_SIZE);
    if (comparison < 0) {
      hi = middle;
    } else if (comparison > 0) {
      lo = middle + 1;
    } else {
      match = middle;
      return JapaneseDictStatus::Found;
    }
  }
  return JapaneseDictStatus::NotFound;
}

JapaneseDictStatus findMatch(SourceState& source, const char* key, size_t& match) {
  if (source.cancellation.requested()) return JapaneseDictStatus::Cancelled;
  if (!source.spxTried) loadSparseIndex(source);
  if (source.cancellation.requested()) return JapaneseDictStatus::Cancelled;
  if (source.spxOk) {
    size_t lo = 0;
    size_t hi = source.recordCount;
    if (narrowWithSparseIndex(source, key, lo, hi)) {
      const JapaneseDictStatus narrowed = binarySearch(source, key, lo, hi, match);
      if (narrowed != JapaneseDictStatus::NotFound) return narrowed;
      DictIndexRecord boundary{};
      bool completeRange = true;
      if (lo > 0) {
        const auto status = readRecord(source, lo - 1, boundary);
        if (status != JapaneseDictStatus::Found) return status;
        completeRange = std::memcmp(boundary.headword, key, SPX_KEY_SIZE) < 0;
      }
      if (completeRange && hi < source.recordCount) {
        const auto status = readRecord(source, hi, boundary);
        if (status != JapaneseDictStatus::Found) return status;
        completeRange = std::memcmp(boundary.headword, key, SPX_KEY_SIZE) > 0;
      }
      if (completeRange) return JapaneseDictStatus::NotFound;
      // A damaged checkpoint must cost performance, never correctness.
    }
  }
  if (source.cancellation.requested()) return JapaneseDictStatus::Cancelled;
  return binarySearch(source, key, 0, source.recordCount, match);
}

bool acceptsPos(uint8_t flags, uint8_t mask) { return mask == 0 || flags == 0 || (flags & mask) != 0; }

JapaneseDictStatus findRunStart(SourceState& source, const char* key, size_t match, size_t& first) {
  first = match;
  while (first > 0 && match - first < MAX_RUN_SCAN) {
    DictIndexRecord previous{};
    const JapaneseDictStatus status = readRecord(source, first - 1, previous);
    if (status != JapaneseDictStatus::Found) return status;
    if (std::memcmp(key, previous.headword, SPX_KEY_SIZE) != 0) return JapaneseDictStatus::Found;
    --first;
  }
  if (first == 0 || match - first < MAX_RUN_SCAN) return JapaneseDictStatus::Found;

  size_t lo = 0;
  size_t hi = first;
  while (lo < hi) {
    const size_t middle = lo + (hi - lo) / 2;
    DictIndexRecord record{};
    const JapaneseDictStatus status = readRecord(source, middle, record);
    if (status != JapaneseDictStatus::Found) return status;
    if (std::memcmp(record.headword, key, SPX_KEY_SIZE) < 0) {
      lo = middle + 1;
    } else {
      hi = middle;
    }
  }
  first = lo;
  return JapaneseDictStatus::Found;
}

JapaneseDictStatus rankSiblings(SourceState& source, const char* key, size_t first, uint8_t posMask,
                                RankedSibling* siblings, size_t& siblingCount) {
  siblingCount = 0;
  const size_t end = std::min(source.recordCount, first + MAX_RUN_SCAN);
  for (size_t index = first; index < end; ++index) {
    DictIndexRecord record{};
    const JapaneseDictStatus status = readRecord(source, index, record);
    if (status != JapaneseDictStatus::Found) return status;
    if (std::memcmp(key, record.headword, SPX_KEY_SIZE) != 0) break;
    if (!acceptsPos(record.posFlags, posMask)) continue;

    size_t insertion = 0;
    while (insertion < siblingCount && siblings[insertion].priority >= record.priority) ++insertion;
    if (insertion >= MAX_SIBLINGS) continue;
    if (siblingCount < MAX_SIBLINGS) ++siblingCount;
    for (size_t move = siblingCount - 1; move > insertion; --move) siblings[move] = siblings[move - 1];
    siblings[insertion] = {record.offset, record.length, record.priority, record.posFlags};
  }
  return siblingCount == 0 ? JapaneseDictStatus::NotFound : JapaneseDictStatus::Found;
}

JapaneseDictStatus findSiblings(SourceState& source, const char* key, uint8_t posMask, RankedSibling* siblings,
                                size_t& siblingCount) {
  size_t match = 0;
  JapaneseDictStatus status = findMatch(source, key, match);
  if (status != JapaneseDictStatus::Found) return status;
  size_t first = 0;
  status = findRunStart(source, key, match, first);
  if (status != JapaneseDictStatus::Found) return status;
  return rankSiblings(source, key, first, posMask, siblings, siblingCount);
}

void clearProbe(DictProbe& probe) { probe = DictProbe{}; }

void clearEntry(DictEntry& entry) { entry.reset(); }

void setEntryMetadata(DictEntry& entry, std::string_view headword, const RankedSibling& sibling, uint8_t source) {
  std::memcpy(entry.headword, headword.data(), headword.size());
  entry.headword[headword.size()] = '\0';
  entry.headwordLength = static_cast<uint8_t>(headword.size());
  entry.priority = sibling.priority;
  entry.sourceDict = source;
  entry.posFlags = sibling.posFlags;
}

bool safeMergeBudget(size_t allocationBytes) {
  if (allocationBytes > UINT32_MAX - DEFINITION_HEAP_HEADROOM) return false;
  const size_t required = allocationBytes + DEFINITION_HEAP_HEADROOM;
  return ESP.getFreeHeap() >= required && ESP.getMaxAllocHeap() >= required;
}

uint64_t fnvUpdate(uint64_t hash, const void* data, size_t length) {
  constexpr uint64_t prime = 1099511628211ULL;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= prime;
  }
  return hash;
}

bool addSourceSignature(SourceState& source, uint64_t& hash, uint8_t* sample, size_t sampleCapacity) {
  hash = fnvUpdate(hash, source.idxPath, std::strlen(source.idxPath));
  std::array<uint8_t, sizeof(uint64_t)> encodedSize{};
  uint64_t size = source.idxSize;
  for (size_t index = 0; index < encodedSize.size(); ++index) {
    encodedSize[index] = static_cast<uint8_t>(size >> (index * 8));
  }
  hash = fnvUpdate(hash, encodedSize.data(), encodedSize.size());

  const size_t sampleSize = std::min(sampleCapacity, source.idxSize);
  if (sampleSize == 0) return true;
  if (!readExact(source.idxFile, 0, sample, sampleSize, source.cancellation)) return false;
  hash = fnvUpdate(hash, sample, sampleSize);
  if (!readExact(source.idxFile, source.idxSize - sampleSize, sample, sampleSize, source.cancellation)) return false;
  hash = fnvUpdate(hash, sample, sampleSize);
  return true;
}

bool makeKey(std::string_view headword, char* key) {
  if (headword.empty() || headword.size() >= DictIndexRecord::HEADWORD_SIZE ||
      std::memchr(headword.data(), 0, headword.size()) != nullptr || !validUtf8(headword.data(), headword.size())) {
    return false;
  }
  std::memset(key, 0, DictIndexRecord::HEADWORD_SIZE);
  std::memcpy(key, headword.data(), headword.size());
  return true;
}
}  // namespace

struct DictIndex::Impl {
  bool identityReadError = false;
  SourceState vocab;
  SourceState grammar;
  SourceState names;
  // 32 * 8 = 256 bytes, session-owned so lookup task stack frames stay small.
  std::array<RankedSibling, MAX_SIBLINGS> siblingScratch{};
  // Reused while hashing each source; 256 bytes would otherwise inflate open()'s stack frame.
  std::array<uint8_t, 256> signatureScratch{};
};

DictIndex::DictIndex() = default;

DictIndex::~DictIndex() { close(); }

JapaneseDictStatus DictIndex::open() {
  close();
  if (cancellationRequested()) return JapaneseDictStatus::Cancelled;
  // One fixed sizeof(Impl) session object owns handles and scratch across calls;
  // it cannot live on a transient lookup stack or in process-lifetime static DRAM.
  impl_ = makeUniqueNoThrow<Impl>();
  if (!impl_) {
    LOG_ERR("DICT", "Dictionary session allocation failed");
    return JapaneseDictStatus::OutOfMemory;
  }

  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) source->cancellation = cancellation_;

  const PathPair* vocabPath = resolvePath(VOCAB_PATHS);
  if (!vocabPath) {
    close();
    return JapaneseDictStatus::Unavailable;
  }
  JapaneseDictStatus status = openSource(impl_->vocab, *vocabPath, DICT_JMDICT);
  if (status != JapaneseDictStatus::Found) {
    close();
    return status;
  }
  availableSources_ = DICT_JMDICT;

  const PathPair* grammarPath = resolvePath(GRAMMAR_PATHS);
  if (grammarPath) {
    status = openSource(impl_->grammar, *grammarPath, DICT_GRAMMAR);
    if (status == JapaneseDictStatus::Found)
      availableSources_ |= DICT_GRAMMAR;
    else {
      impl_->identityReadError = status == JapaneseDictStatus::ReadError;
      impl_->grammar.close();
    }
  }
  if (cancellationRequested()) {
    close();
    return JapaneseDictStatus::Cancelled;
  }
  const PathPair* namesPath = resolvePath(NAMES_PATHS);
  if (namesPath) {
    status = openSource(impl_->names, *namesPath, DICT_NAMES);
    if (status == JapaneseDictStatus::Found)
      availableSources_ |= DICT_NAMES;
    else {
      impl_->identityReadError = impl_->identityReadError || status == JapaneseDictStatus::ReadError;
      impl_->names.close();
    }
  }

  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (cancellationRequested()) {
      close();
      return JapaneseDictStatus::Cancelled;
    }
    if (source->available) prepareAccelerators(*source);
  }

  if (cancellationRequested()) {
    close();
    return JapaneseDictStatus::Cancelled;
  }
  uint64_t hash = 14695981039346656037ULL;
  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (source->available &&
        !addSourceSignature(*source, hash, impl_->signatureScratch.data(), impl_->signatureScratch.size())) {
      const auto status = readFailureStatus(*source);
      close();
      return status;
    }
  }
  signature_ = hash;
  return JapaneseDictStatus::Found;
}

JapaneseDictStatus DictIndex::probeExact(std::string_view headword, DictProbe& out, uint8_t dictMask, uint8_t posMask) {
  clearProbe(out);
  if (cancellationRequested()) return JapaneseDictStatus::Cancelled;
  if (!impl_ || availableSources_ == 0) return JapaneseDictStatus::Unavailable;
  char key[DictIndexRecord::HEADWORD_SIZE];
  if (!makeKey(headword, key)) return JapaneseDictStatus::NotFound;

  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (!source->available || (dictMask & source->source) == 0) continue;
    size_t count = 0;
    const JapaneseDictStatus status = findSiblings(*source, key, posMask, impl_->siblingScratch.data(), count);
    if (status == JapaneseDictStatus::Found) {
      std::memcpy(out.headword, key, sizeof(out.headword));
      out.priority = impl_->siblingScratch[0].priority;
      out.sourceDict = source->source;
      out.posFlags = impl_->siblingScratch[0].posFlags;
      return JapaneseDictStatus::Found;
    }
    if (status != JapaneseDictStatus::NotFound) return status;
  }
  return JapaneseDictStatus::NotFound;
}

JapaneseDictStatus DictIndex::checkUsuallyKana(std::string_view headword, bool& found, uint8_t dictMask,
                                               uint8_t posMask) {
  found = false;
  if (cancellationRequested()) return JapaneseDictStatus::Cancelled;
  if (!impl_ || availableSources_ == 0) return JapaneseDictStatus::Unavailable;
  char key[DictIndexRecord::HEADWORD_SIZE];
  if (!makeKey(headword, key)) return JapaneseDictStatus::NotFound;
  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (!source->available || (dictMask & source->source) == 0) continue;
    size_t count = 0;
    const auto status = findSiblings(*source, key, posMask, impl_->siblingScratch.data(), count);
    if (status == JapaneseDictStatus::NotFound) continue;
    if (status != JapaneseDictStatus::Found) return status;
    // One bounded, fallible scratch allocation per probe; no merged definitions
    // or task-stack-sized sense buffers. Released before the next page position.
    constexpr size_t kChunkBytes = 128;
    constexpr std::string_view kMarker = "[kana]";
    auto scratch = makeUniqueNoThrow<char[]>(kChunkBytes);
    if (!scratch) {
      LOG_ERR("DICT", "Kana marker scratch allocation failed");
      return JapaneseDictStatus::OutOfMemory;
    }
    size_t selected = 0;
    for (size_t index = 0; index < count && selected < MAX_MERGED_ENTRIES; ++index) {
      const auto& sense = impl_->siblingScratch[index];
      if (sense.length > MAX_DEFINITION_BYTES) continue;
      ++selected;
      size_t offset = 0;
      size_t retained = 0;
      while (offset < sense.length) {
        if (cancellationRequested()) return JapaneseDictStatus::Cancelled;
        const size_t bytes = std::min(kChunkBytes - retained, static_cast<size_t>(sense.length) - offset);
        if (!readExact(source->datFile, sense.offset + offset, scratch.get() + retained, bytes, source->cancellation)) {
          if (!cancellationRequested()) LOG_ERR("DICT", "Kana marker definition read failed");
          return readFailureStatus(*source);
        }
        const size_t available = retained + bytes;
        if (std::string_view(scratch.get(), available).find(kMarker) != std::string_view::npos) {
          found = true;
          return JapaneseDictStatus::Found;
        }
        offset += bytes;
        retained = std::min(available, kMarker.size() - 1);
        std::memmove(scratch.get(), scratch.get() + available - retained, retained);
      }
    }
    return JapaneseDictStatus::Found;
  }
  return JapaneseDictStatus::NotFound;
}

JapaneseDictStatus DictIndex::lookupExact(std::string_view headword, DictEntry& out, uint8_t dictMask,
                                          uint8_t posMask) {
  clearEntry(out);
  if (cancellationRequested()) return JapaneseDictStatus::Cancelled;
  if (!impl_ || availableSources_ == 0) return JapaneseDictStatus::Unavailable;
  char key[DictIndexRecord::HEADWORD_SIZE];
  if (!makeKey(headword, key)) return JapaneseDictStatus::NotFound;

  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (!source->available || (dictMask & source->source) == 0) continue;
    size_t count = 0;
    JapaneseDictStatus status = findSiblings(*source, key, posMask, impl_->siblingScratch.data(), count);
    if (status == JapaneseDictStatus::NotFound) continue;
    if (status != JapaneseDictStatus::Found) return status;

    std::array<size_t, MAX_MERGED_ENTRIES> selected{};
    size_t selectedCount = 0;
    for (size_t index = 0; index < count && selectedCount < selected.size(); ++index) {
      if (impl_->siblingScratch[index].length > MAX_DEFINITION_BYTES) {
        LOG_ERR("DICT", "Skipping oversized dictionary sense (%u bytes)",
                static_cast<unsigned>(impl_->siblingScratch[index].length));
        continue;
      }
      selected[selectedCount++] = index;
    }
    if (selectedCount == 0) return JapaneseDictStatus::NotFound;

    const RankedSibling& best = impl_->siblingScratch[selected[0]];
    // At most 16,385 bytes. The result must outlive this call, so a task-stack
    // buffer is unsuitable; this exact allocation becomes the owned result.
    auto bestDefinition = makeUniqueNoThrow<char[]>(static_cast<size_t>(best.length) + 1);
    if (!bestDefinition) {
      LOG_ERR("DICT", "Best dictionary sense allocation failed (%u bytes)", static_cast<unsigned>(best.length + 1));
      return JapaneseDictStatus::OutOfMemory;
    }
    if (!readExact(source->datFile, best.offset, bestDefinition.get(), best.length, source->cancellation)) {
      if (!cancellationRequested()) LOG_ERR("DICT", "Best dictionary sense read failed");
      return readFailureStatus(*source);
    }
    bestDefinition[best.length] = '\0';
    out.definition = std::move(bestDefinition);
    out.definitionLength = best.length;
    setEntryMetadata(out, headword, best, source->source);
    if (selectedCount == 1) return JapaneseDictStatus::Found;

    size_t mergedLength = 0;
    for (size_t index = 0; index < selectedCount; ++index) {
      mergedLength += impl_->siblingScratch[selected[index]].length;
      if (index != 0) mergedLength += sizeof(MERGE_SEPARATOR) - 1;
    }
    const size_t mergedAllocation = mergedLength + 1;
    if (!safeMergeBudget(mergedAllocation)) {
      LOG_ERR("DICT", "Skipping dictionary sense merge (%u bytes, free=%u, maxAlloc=%u)",
              static_cast<unsigned>(mergedAllocation), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return JapaneseDictStatus::Found;
    }
    // At most 81,945 bytes for five capped senses and four separators. This is
    // one exact, fallible allocation; failure retains the readable best sense.
    auto merged = makeUniqueNoThrow<char[]>(mergedAllocation);
    if (!merged) {
      LOG_ERR("DICT", "Dictionary sense merge allocation failed (%u bytes); keeping best sense",
              static_cast<unsigned>(mergedAllocation));
      return JapaneseDictStatus::Found;
    }

    size_t writeOffset = best.length;
    std::memcpy(merged.get(), out.definition.get(), best.length);
    for (size_t index = 1; index < selectedCount; ++index) {
      std::memcpy(merged.get() + writeOffset, MERGE_SEPARATOR, sizeof(MERGE_SEPARATOR) - 1);
      writeOffset += sizeof(MERGE_SEPARATOR) - 1;
      const RankedSibling& sibling = impl_->siblingScratch[selected[index]];
      if (!readExact(source->datFile, sibling.offset, merged.get() + writeOffset, sibling.length,
                     source->cancellation)) {
        if (!cancellationRequested()) LOG_ERR("DICT", "Dictionary sense merge read failed");
        clearEntry(out);
        return readFailureStatus(*source);
      }
      writeOffset += sibling.length;
    }
    merged[writeOffset] = '\0';
    out.definition = std::move(merged);
    out.definitionLength = writeOffset;
    return JapaneseDictStatus::Found;
  }
  return JapaneseDictStatus::NotFound;
}

uint8_t DictIndex::availableSources() const { return availableSources_; }

uint64_t DictIndex::signature() const { return signature_; }

void DictIndex::close() {
  if (impl_) {
    impl_->names.close();
    impl_->grammar.close();
    impl_->vocab.close();
    impl_.reset();
  }
  availableSources_ = 0;
  signature_ = 0;
}

DictionaryScanIdentityStatus DictIndex::beginScanIdentity(DictionaryScanIdentityState& state) {
  state.start(1);
  if (!impl_ || !availableSources_) return state.fail(DictionaryScanIdentityStatus::Unavailable);
  if (impl_->identityReadError) return state.fail(DictionaryScanIdentityStatus::ReadError);
  unsigned ordinal = 0;
  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    state.number(1u << ordinal);
    state.number(source->available);
    if (source->available) {
      if (source->idxFile.fileSize64() > UINT32_MAX) return state.fail(DictionaryScanIdentityStatus::Unavailable);
      state.presentMask_ |= 1u << ordinal;
      state.japanesePaths_[ordinal] = source->idxPath;
      state.sizes_[ordinal] = source->idxSize;
      state.dataSizes_[ordinal] = source->datFile.fileSize64();
      state.text(source->idxPath, std::strlen(source->idxPath));
      state.text(source->datPath, std::strlen(source->datPath));
      state.number(source->idxSize);
      state.number(state.dataSizes_[ordinal]);
    }
    ++ordinal;
  }
  return state.status_;
}

bool DictIndex::resumeScanIdentity(DictionaryScanIdentityState& state) {
  if (!impl_ || impl_->identityReadError || state.backend_ != 1 || state.presentMask_ != availableSources_)
    return false;
  unsigned ordinal = 0;
  for (SourceState* source : {&impl_->vocab, &impl_->grammar, &impl_->names}) {
    if (source->available &&
        (state.japanesePaths_[ordinal] != source->idxPath || state.sizes_[ordinal] != source->idxSize ||
         state.dataSizes_[ordinal] != source->datFile.fileSize64()))
      return false;
    ++ordinal;
  }
  return true;
}

DictionaryScanIdentityStatus DictIndex::stepScanIdentity(DictionaryScanIdentityState& state, size_t byteBudget) {
  if (state.status_ != DictionaryScanIdentityStatus::Pending) return state.status_;
  if (!resumeScanIdentity(state)) return state.fail(DictionaryScanIdentityStatus::Unavailable);
  SourceState* sources[] = {&impl_->vocab, &impl_->grammar, &impl_->names};
  while (state.source_ < 3 &&
         (!(state.presentMask_ & (1u << state.source_)) || state.offset_ == state.sizes_[state.source_])) {
    ++state.source_;
    state.offset_ = 0;
  }
  if (state.source_ == 3) {
    state.status_ = DictionaryScanIdentityStatus::Ready;
    return state.status_;
  }
  const size_t bytes = std::min(
      {byteBudget, impl_->signatureScratch.size(), static_cast<size_t>(state.sizes_[state.source_] - state.offset_)});
  if (!bytes) return state.status_;
  if (!readExact(sources[state.source_]->idxFile, state.offset_, impl_->signatureScratch.data(), bytes))
    return state.fail(DictionaryScanIdentityStatus::ReadError);
  state.bytes(impl_->signatureScratch.data(), bytes);
  state.offset_ += bytes;
  // Publication is a separate cancellable step, including after the last read.
  return state.status_;
}
