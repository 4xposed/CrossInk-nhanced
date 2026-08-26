#include "ReviewStateStore.h"

#include "AnkiDeck.h"

#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <limits>
namespace {
constexpr char kLogTag[] = "AnkiState";
constexpr uint16_t kStateVersion = 2;
constexpr uint16_t kStateHeaderSize = 24;
constexpr uint32_t kStateRecordSize = 8;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}
uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
uint64_t readLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(data[index]) << (index * 8);
  return value;
}
void writeLe16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}
void writeLe32(uint8_t* data, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) data[index] = static_cast<uint8_t>(value >> (index * 8));
}
void writeLe64(uint8_t* data, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) data[index] = static_cast<uint8_t>(value >> (index * 8));
}
bool readExactly(FsFile& file, void* data, size_t size) {
  return file.read(data, size) == static_cast<int>(size);
}
bool writeExactly(FsFile& file, const void* data, size_t size) {
  return file.write(data, size) == size;
}
void encodeStateHeader(uint8_t* data, uint64_t deckId, uint32_t count, uint32_t reviewCount) {
  data[0] = 'C';
  data[1] = 'K';
  data[2] = 'R';
  data[3] = 'S';
  writeLe16(data + 4, kStateVersion);
  writeLe16(data + 6, kStateHeaderSize);
  writeLe64(data + 8, deckId);
  writeLe32(data + 16, count);
  writeLe32(data + 20, reviewCount);
}
void encodeStateRecord(uint8_t* data, const ReviewState& state) {
  writeLe32(data, state.dueDay);
  writeLe16(data + 4, state.intervalDays);
  data[6] = static_cast<uint8_t>(state.kind);
  data[7] = state.flags;
}
bool decodeStateRecord(const uint8_t* data, ReviewState& state) {
  const uint8_t kind = data[6];
  if (kind > static_cast<uint8_t>(ReviewKind::Learning) || readLe16(data + 4) == 0) return false;
  state.dueDay = readLe32(data);
  state.intervalDays = readLe16(data + 4);
  state.kind = static_cast<ReviewKind>(kind);
  state.flags = data[7];
  return true;
}
}  // namespace

bool ReviewStateStore::open(AnkiDeck& deck) {
  endStream();
  deck_ = nullptr;
  statePath_.clear();
  activeStatePath_.clear();
  clearPending();
  reviewCount_ = 0;
  reviewCountDirty_ = false;
  updatesSinceFlush_ = 0;
  hasFlushTimestamp_ = false;
  if (deck.cardCount_ == 0 || deck.deckId_ == 0) {
    LOG_ERR(kLogTag, "Cannot open state for unloaded deck");
    return false;
  }
  if (!deck.validateFullIndex()) {
    LOG_ERR(kLogTag, "Deck index is invalid");
    return false;
  }
  deck_ = &deck;
  deckId_ = deck.deckId_;
  cardCount_ = deck.cardCount_;
  statePath_ = deck.getCachePath() + "/review.bin";

  uint32_t persistedReviewCount = 0;
  const StateFileStatus primaryStatus = inspectStateFile(statePath_, &persistedReviewCount);
  if (primaryStatus == StateFileStatus::Valid) {
    reviewCount_ = persistedReviewCount;
    activeStatePath_ = statePath_;
    return true;
  }
  if (primaryStatus == StateFileStatus::Mismatch) {
    LOG_ERR(kLogTag, "Review state belongs to a different deck");
    return initializeState();
  }
  if (primaryStatus == StateFileStatus::Malformed) {
    LOG_ERR(kLogTag, "Review state is malformed; attempting backup");
  }

  const std::string backupPath = statePath_ + ".bak";
  const StateFileStatus backupStatus = inspectStateFile(backupPath, &persistedReviewCount);
  if (backupStatus == StateFileStatus::Valid) {
    LOG_DBG(kLogTag, "Recovered review state from backup");
    reviewCount_ = persistedReviewCount;
    activeStatePath_ = backupPath;
    return true;
  }
  if (backupStatus == StateFileStatus::Mismatch) {
    LOG_ERR(kLogTag, "Review state backup belongs to a different deck");
  } else if (backupStatus == StateFileStatus::Malformed) {
    LOG_ERR(kLogTag, "Review state backup is malformed");
  }
  return initializeState();
}

ReviewStateStore::StateFileStatus ReviewStateStore::inspectStateFile(const std::string& path,
                                                                      uint32_t* reviewCount) const {
  if (!Storage.exists(path.c_str())) return StateFileStatus::Missing;
  FsFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) return StateFileStatus::Malformed;
  const uint64_t expectedSize = kStateHeaderSize + static_cast<uint64_t>(cardCount_) * kStateRecordSize;
  std::array<uint8_t, kStateHeaderSize> header{};
  const bool headerRead = file.fileSize64() == expectedSize && readExactly(file, header.data(), header.size());
  bool valid = headerRead && header[0] == 'C' && header[1] == 'K' && header[2] == 'R' && header[3] == 'S' &&
               readLe16(header.data() + 4) == kStateVersion && readLe16(header.data() + 6) == kStateHeaderSize;
  const bool identityMatches = valid && readLe64(header.data() + 8) == deckId_ && readLe32(header.data() + 16) == cardCount_;
  if (valid && identityMatches) {
    std::array<uint8_t, kStateRecordSize> record{};
    for (uint32_t index = 0; index < cardCount_; ++index) {
      if (!readExactly(file, record.data(), record.size())) {
        valid = false;
        break;
      }
      ReviewState decoded;
      if (!decodeStateRecord(record.data(), decoded)) {
        valid = false;
        break;
      }
    }
  }
  file.close();
  if (!valid) return StateFileStatus::Malformed;
  if (identityMatches && reviewCount != nullptr) *reviewCount = readLe32(header.data() + 20);
  return identityMatches ? StateFileStatus::Valid : StateFileStatus::Mismatch;
}

bool ReviewStateStore::writeInitialStateRecord(uint32_t index, const AnkiDeckMetadata& metadata, void* context) {
  auto* writeContext = static_cast<InitialStateWriteContext*>(context);
  if (writeContext == nullptr || writeContext->store == nullptr || writeContext->file == nullptr) return false;

  ReviewState state{};
  state.flags = 0;
  state.kind = static_cast<ReviewKind>(metadata.initialKind);
  state.intervalDays = metadata.importedIntervalDays == 0 ? 1 : metadata.importedIntervalDays;
  state.dueDay = 0;
  if (const PendingUpdate* pending = writeContext->store->pendingFor(index); pending != nullptr) state = pending->state;

  std::array<uint8_t, kStateRecordSize> recordBytes{};
  encodeStateRecord(recordBytes.data(), state);
  return writeExactly(*writeContext->file, recordBytes.data(), recordBytes.size());
}

bool ReviewStateStore::initializeState() {
  if (deck_ == nullptr || !Storage.ensureDirectoryExists(deck_->getCachePath().c_str())) {
    LOG_ERR(kLogTag, "Could not create review state directory");
    return false;
  }
  activeStatePath_.clear();
  return writeStateAtomically(false);
}

const ReviewStateStore::PendingUpdate* ReviewStateStore::pendingFor(uint32_t index) const {
  for (uint32_t pendingIndex = 0; pendingIndex < pendingCount_; ++pendingIndex) {
    if (pending_[pendingIndex].index == index) return &pending_[pendingIndex];
  }
  return nullptr;
}

bool ReviewStateStore::readPersisted(uint32_t index, ReviewState& out) const {
  if (activeStatePath_.empty() || index >= cardCount_) return false;
  FsFile file;
  if (!Storage.openFileForRead(kLogTag, activeStatePath_, file)) {
    LOG_ERR(kLogTag, "Could not open review state");
    return false;
  }
  std::array<uint8_t, kStateRecordSize> bytes{};
  const uint64_t offset = kStateHeaderSize + static_cast<uint64_t>(index) * kStateRecordSize;
  const bool read = file.seek64(offset) && readExactly(file, bytes.data(), bytes.size());
  file.close();
  if (!read || !decodeStateRecord(bytes.data(), out)) {
    LOG_ERR(kLogTag, "Could not read review state record");
    return false;
  }
  return true;
}

bool ReviewStateStore::read(uint32_t index, ReviewState& out) {
  endStream();
  if (index >= cardCount_) return false;
  if (const PendingUpdate* pending = pendingFor(index); pending != nullptr) {
    out = pending->state;
    return true;
  }
  return readPersisted(index, out);
}

bool ReviewStateStore::beginStream() {
  endStream();
  if (deck_ == nullptr || activeStatePath_.empty() ||
      !Storage.openFileForRead(kLogTag, activeStatePath_, streamFile_) ||
      !streamFile_.seek64(kStateHeaderSize)) {
    LOG_ERR(kLogTag, "Could not open review state stream");
    endStream();
    return false;
  }
  nextStreamIndex_ = 0;
  return true;
}

bool ReviewStateStore::readNext(ReviewState& out) {
  if (!streamFile_.isOpen() || nextStreamIndex_ >= cardCount_) {
    endStream();
    return false;
  }
  std::array<uint8_t, kStateRecordSize> bytes{};
  const uint32_t index = nextStreamIndex_++;
  const bool read = readExactly(streamFile_, bytes.data(), bytes.size()) && decodeStateRecord(bytes.data(), out);
  if (!read) {
    LOG_ERR(kLogTag, "Could not stream review state record");
    endStream();
    return false;
  }
  if (const PendingUpdate* pending = pendingFor(index); pending != nullptr) out = pending->state;
  if (nextStreamIndex_ == cardCount_) endStream();
  return true;
}

void ReviewStateStore::endStream() {
  if (streamFile_.isOpen()) streamFile_.close();
  nextStreamIndex_ = 0;
}

bool ReviewStateStore::replace(uint32_t index, const ReviewState& value, uint64_t nowMilliseconds) {
  if (index >= cardCount_ || value.intervalDays == 0 ||
      static_cast<uint8_t>(value.kind) > static_cast<uint8_t>(ReviewKind::Learning)) return false;
  for (uint32_t pendingIndex = 0; pendingIndex < pendingCount_; ++pendingIndex) {
    if (pending_[pendingIndex].index == index) {
      pending_[pendingIndex].state = value;
      return true;
    }
  }
  if (pendingCount_ == kMaxPendingUpdates && !flush()) return false;
  pending_[pendingCount_++] = PendingUpdate{index, value};
  if (!hasFlushTimestamp_) {
    lastFlushMilliseconds_ = nowMilliseconds;
    hasFlushTimestamp_ = true;
  }
  return true;
}

bool ReviewStateStore::replaceAndAdvance(uint32_t index, const ReviewState& value, uint64_t nowMilliseconds) {
  if (deck_ == nullptr || reviewCount_ == std::numeric_limits<uint32_t>::max()) {
    LOG_ERR(kLogTag, "Could not apply grade at review count limit");
    return false;
  }
  return replace(index, value, nowMilliseconds) && advanceReviewCount(nowMilliseconds);
}

bool ReviewStateStore::advanceReviewCount(uint64_t nowMilliseconds) {
  if (deck_ == nullptr || reviewCount_ == std::numeric_limits<uint32_t>::max()) {
    LOG_ERR(kLogTag, "Could not advance review count");
    return false;
  }
  ++reviewCount_;
  reviewCountDirty_ = true;
  if (!hasFlushTimestamp_) {
    lastFlushMilliseconds_ = nowMilliseconds;
    hasFlushTimestamp_ = true;
  }
  ++updatesSinceFlush_;
  return true;
}

bool ReviewStateStore::shouldFlush(uint64_t nowMilliseconds) const {
  if (pendingCount_ == 0 && !reviewCountDirty_) return false;
  return pendingCount_ >= kMaxPendingUpdates || updatesSinceFlush_ >= kMaxPendingUpdates ||
         (hasFlushTimestamp_ && nowMilliseconds >= lastFlushMilliseconds_ &&
          nowMilliseconds - lastFlushMilliseconds_ >= kFlushIntervalMilliseconds);
}

bool ReviewStateStore::flushIfDue(uint64_t nowMilliseconds) {
  return !shouldFlush(nowMilliseconds) || flush();
}

bool ReviewStateStore::writeStateAtomically(bool fromExisting) {
  const std::string temporaryPath = statePath_ + ".tmp";
  const std::string backupPath = statePath_ + ".bak";
  if (deck_ == nullptr || !Storage.ensureDirectoryExists(deck_->getCachePath().c_str())) {
    LOG_ERR(kLogTag, "Could not create review state directory");
    return false;
  }
  const bool recoveringFromBackup = fromExisting && activeStatePath_ == backupPath;
  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
    LOG_ERR(kLogTag, "Could not remove stale review temp");
    return false;
  }
  FsFile temporary;
  if (!Storage.openFileForWrite(kLogTag, temporaryPath, temporary)) {
    LOG_ERR(kLogTag, "Could not open review temp");
    return false;
  }
  std::array<uint8_t, kStateHeaderSize> header{};
  encodeStateHeader(header.data(), deckId_, cardCount_, reviewCount_);
  bool wrote = writeExactly(temporary, header.data(), header.size());

  FsFile source;
  if (wrote && fromExisting) {
    if (!Storage.openFileForRead(kLogTag, activeStatePath_, source) || !source.seek64(kStateHeaderSize)) {
      wrote = false;
    }
  }
  if (wrote && fromExisting) {
    std::array<uint8_t, kStateRecordSize> recordBytes{};
    for (uint32_t index = 0; wrote && index < cardCount_; ++index) {
      ReviewState state;
      wrote = readExactly(source, recordBytes.data(), recordBytes.size()) && decodeStateRecord(recordBytes.data(), state);
      if (wrote) {
        if (const PendingUpdate* pending = pendingFor(index); pending != nullptr) state = pending->state;
        encodeStateRecord(recordBytes.data(), state);
        wrote = writeExactly(temporary, recordBytes.data(), recordBytes.size());
      }
    }
  } else if (wrote) {
    InitialStateWriteContext context{this, &temporary};
    wrote = deck_->streamMetadata(&ReviewStateStore::writeInitialStateRecord, &context);
  }
  if (source.isOpen()) source.close();
  if (!wrote || !temporary.sync() || !temporary.close()) {
    LOG_ERR(kLogTag, "Could not write review temp");
    temporary.close();
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (recoveringFromBackup) {
    if (Storage.exists(statePath_.c_str()) && !Storage.remove(statePath_.c_str())) {
      LOG_ERR(kLogTag, "Could not remove corrupt review state");
      Storage.remove(temporaryPath.c_str());
      return false;
    }
    if (!Storage.rename(temporaryPath.c_str(), statePath_.c_str())) {
      LOG_ERR(kLogTag, "Could not replace recovered review state");
      if (Storage.exists(backupPath.c_str()) && !Storage.exists(statePath_.c_str())) Storage.rename(backupPath.c_str(), statePath_.c_str());
      Storage.remove(temporaryPath.c_str());
      return false;
    }
  } else {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR(kLogTag, "Could not remove old review backup");
      Storage.remove(temporaryPath.c_str());
      return false;
    }
    if (Storage.exists(statePath_.c_str()) && !Storage.rename(statePath_.c_str(), backupPath.c_str())) {
      LOG_ERR(kLogTag, "Could not rotate review state");
      Storage.remove(temporaryPath.c_str());
      return false;
    }
    if (!Storage.rename(temporaryPath.c_str(), statePath_.c_str())) {
      LOG_ERR(kLogTag, "Could not replace review state");
      if (Storage.exists(backupPath.c_str()) && !Storage.exists(statePath_.c_str())) Storage.rename(backupPath.c_str(), statePath_.c_str());
      Storage.remove(temporaryPath.c_str());
      return false;
    }
  }
  activeStatePath_ = statePath_;
  clearPending();
  reviewCountDirty_ = false;
  updatesSinceFlush_ = 0;
  hasFlushTimestamp_ = false;
  return true;
}

bool ReviewStateStore::flush() {
  if (deck_ == nullptr) return false;
  endStream();
  return writeStateAtomically(!activeStatePath_.empty());
}

bool ReviewStateStore::onExit() {
  endStream();
  return (pendingCount_ == 0 && !reviewCountDirty_) || flush();
}
uint32_t ReviewStateStore::cardCount() const { return cardCount_; }
uint32_t ReviewStateStore::reviewCount() const { return reviewCount_; }

void ReviewStateStore::clearPending() { pendingCount_ = 0; }
