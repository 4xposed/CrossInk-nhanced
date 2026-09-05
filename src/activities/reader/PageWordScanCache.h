#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "PageWordScanner.h"

struct PageWordScanCacheIdentity {
  DictionaryBackendKind backend = DictionaryBackendKind::StarDict;
  uint16_t spine = 0;
  uint16_t page = 0;
  uint32_t glyphHash = 0;
  uint64_t dictionarySignature = 0;
  // Current immutable PageTextSourceView glyph count. Cache records are
  // validated against it before any loaded state is published.
  uint16_t sourceGlyphCount = 0;
};

class PageWordScanCache {
 public:
  static constexpr uint32_t kMagic = UINT32_C(0x534c5743);
  static constexpr uint8_t kVersion = 2;
  static constexpr uint16_t kCompleteFlag = 1;
  static constexpr size_t kHeaderSize = 32;
  static constexpr size_t kRecordSize = 8;

  // Returns false for every absent, stale, corrupt, OOM, or I/O cache miss and
  // leaves this object empty. A true return owns one exact candidate array.
  bool load(const char* path, const PageWordScanCacheIdentity& identity);
  // The scanner is borrowed only for this call. Only complete, non-truncated
  // scans can be atomically published.
  bool save(const char* path, const PageWordScanCacheIdentity& identity, const PageWordScanner& scanner,
            uint16_t cursor);
  // Rewrite a validated cache hit with a new cursor without reopening the
  // backend scanner. The in-memory candidate snapshot remains immutable.
  bool saveLoaded(const char* path, const PageWordScanCacheIdentity& identity, uint16_t cursor);

  void clear();
  uint16_t candidateCount() const { return candidateCount_; }
  uint16_t cursor() const { return cursor_; }
  // Returned storage is immutable and stable until the next load(), clear(),
  // or destruction.
  const PageWordCandidate* candidate(uint16_t index) const;

 private:
  bool saveCandidates(const char* path, const PageWordScanCacheIdentity& identity, const PageWordCandidate* candidates,
                      uint16_t candidateCount, uint16_t cursor);

  std::unique_ptr<PageWordCandidate[]> candidates_;
  uint16_t candidateCount_ = 0;
  uint16_t cursor_ = 0;
};

static_assert(PageWordScanCache::kHeaderSize == 32, "wlscan v2 header size is part of the disk contract");
static_assert(PageWordScanCache::kRecordSize == sizeof(PageWordCandidate),
              "wlscan candidate record must remain eight bytes");
static_assert(static_cast<uint8_t>(DictionaryBackendKind::StarDict) == 0,
              "wlscan v2 freezes the StarDict backend byte");
static_assert(static_cast<uint8_t>(DictionaryBackendKind::Japanese) == 1,
              "wlscan v2 freezes the Japanese backend byte");
