#pragma once

#include <cstdint>
#include <memory>

#include "PageTextSource.h"
#include "util/DictionaryEngineTypes.h"

struct PageWordCandidate {
  uint16_t firstGlyph = 0;
  uint8_t glyphCount = 0;
  uint8_t matchedBytes = 0;
  uint16_t firstPageWord = 0;
  uint16_t lastPageWord = 0;
};

static_assert(sizeof(PageWordCandidate) == 8, "Page-word candidates must stay compact on ESP32-C3");

struct DictionaryProbeFn {
  void* context = nullptr;
  DictionaryStatus (*call)(void*, const DictionaryQuery&, DictionaryProbeResult&) = nullptr;
};

struct PageWordScannerMemoryRecoveryFn {
  void* context = nullptr;
  void (*release)(void*) = nullptr;
};

class PageWordScanner {
 public:
  // source and probe are borrowed. The source glyph storage must remain
  // immutable, and both glyph/callback storage must outlive scanning,
  // restart(), and every candidate read until clear()/destruction.
  // geometryOnlyStarDict skips redundant dictionary probes for whole-word touch
  // selection; Japanese still probes to determine word boundaries. Legacy callers
  // keep probe/error semantics by leaving this option false.
  DictionaryStatus begin(PageTextSourceView source, DictionaryBackendKind backend, DictionaryProbeFn probe,
                         PageWordScannerMemoryRecoveryFn memoryRecovery = {}, bool geometryOnlyStarDict = false);
  DictionaryStatus stepOne();
  bool done() const { return done_; }
  // True only when scanning reached the natural end of the source without a
  // terminal backend/allocation error. It does not imply the full-capacity
  // allocation succeeded; callers that persist results must also reject
  // truncated().
  bool completedSuccessfully() const { return initialized_ && done_ && terminalStatus_ == DictionaryStatus::Found; }
  bool truncated() const { return truncated_; }
  bool cacheable() const { return completedSuccessfully() && !truncated_ && markerChecksComplete_; }
  bool hasProcessedGlyph(uint16_t glyphIndex) const { return initialized_ && scanPos_ > glyphIndex; }
  uint16_t candidateCount() const { return candidateCount_; }
  // Returned storage is immutable and stable until begin(), restart(), clear(),
  // or destruction. Later successful stepOne() calls only append to the array.
  const PageWordCandidate* candidate(uint16_t index) const;
  DictionaryStatus restart();
  void clear();

 private:
  DictionaryStatus allocateCandidates(bool preferFullCapacity);
  DictionaryStatus scanStarDict();
  DictionaryStatus scanJapanese();

  PageTextSourceView source_{};
  DictionaryProbeFn probe_{};
  std::unique_ptr<PageWordCandidate[]> candidates_;
  DictionaryBackendKind backend_ = DictionaryBackendKind::StarDict;
  DictionaryStatus terminalStatus_ = DictionaryStatus::Found;
  uint16_t candidateCapacity_ = 0;
  uint16_t candidateCount_ = 0;
  uint16_t scanPos_ = 0;
  uint16_t skipUntil_ = 0;
  bool geometryOnlyStarDict_ = false;
  bool markerChecksComplete_ = true;
  bool initialized_ = false;
  bool done_ = false;
  bool truncated_ = false;
};
