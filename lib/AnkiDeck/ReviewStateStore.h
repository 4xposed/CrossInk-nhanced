#pragma once

#include "AnkiDeckTypes.h"
#include <HalStorage.h>


#include <cstdint>
#include <string>

class AnkiDeck;
struct AnkiDeckMetadata;

class ReviewStateStore {
 public:
  static constexpr uint32_t kMaxPendingUpdates = 10;
  static constexpr uint64_t kFlushIntervalMilliseconds = 5ULL * 60ULL * 1000ULL;

  bool open(AnkiDeck& deck);
  bool read(uint32_t index, ReviewState& out);
  bool beginStream();
  bool readNext(ReviewState& out);
  void endStream();
  bool replace(uint32_t index, const ReviewState& value, uint64_t nowMilliseconds);
  bool replaceAndAdvance(uint32_t index, const ReviewState& value, uint64_t nowMilliseconds);
  bool advanceReviewCount(uint64_t nowMilliseconds);
  bool shouldFlush(uint64_t nowMilliseconds) const;
  bool flushIfDue(uint64_t nowMilliseconds);
  bool flush();
  bool onExit();
  uint32_t cardCount() const;
  uint32_t reviewCount() const;

 private:
  struct PendingUpdate {
    uint32_t index = 0;
    ReviewState state{};
  };

  enum class StateFileStatus : uint8_t { Missing, Valid, Mismatch, Malformed };

  AnkiDeck* deck_ = nullptr;
  std::string statePath_;
  std::string activeStatePath_;
  uint64_t deckId_ = 0;
  uint32_t cardCount_ = 0;
  uint32_t reviewCount_ = 0;
  bool reviewCountDirty_ = false;
  FsFile streamFile_;
  uint32_t nextStreamIndex_ = 0;
  PendingUpdate pending_[kMaxPendingUpdates]{};
  uint32_t pendingCount_ = 0;
  uint32_t updatesSinceFlush_ = 0;
  uint64_t lastFlushMilliseconds_ = 0;
  bool hasFlushTimestamp_ = false;

  StateFileStatus inspectStateFile(const std::string& path, uint32_t* reviewCount = nullptr) const;
  bool initializeState();
  struct InitialStateWriteContext {
    ReviewStateStore* store = nullptr;
    FsFile* file = nullptr;
  };

  bool writeStateAtomically(bool fromExisting);
  static bool writeInitialStateRecord(uint32_t index, const AnkiDeckMetadata& metadata, void* context);
  bool readPersisted(uint32_t index, ReviewState& out) const;
  const PendingUpdate* pendingFor(uint32_t index) const;
  void clearPending();
};
