#include "DictionaryLookupFlow.h"

#include <Utf8.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
uint32_t nextGeneration(const uint32_t current) {
  return current == std::numeric_limits<uint32_t>::max() ? 1 : current + 1;
}
}  // namespace

DictionaryLookupHistoryKind dictionaryLookupHistoryKind(const DictionaryResult& result, const bool suggestion) {
  if (suggestion) return DictionaryLookupHistoryKind::Suggestion;
  if (result.alternate) return DictionaryLookupHistoryKind::AltForm;
  return result.transformed ? DictionaryLookupHistoryKind::Stem : DictionaryLookupHistoryKind::Direct;
}

DictionaryStatus normalizeRetainedDictionaryLookupText(const DictionaryBackendKind backend, DictionaryOwnedText& text,
                                                       char* const scratch, const size_t scratchCapacity) {
  if (text.empty()) return DictionaryStatus::NotFound;
  if (backend != DictionaryBackendKind::StarDict) return DictionaryStatus::Found;
  if (!scratch || scratchCapacity == 0) return DictionaryStatus::ReadError;
  if (text.length() >= scratchCapacity) return DictionaryStatus::NotFound;

  size_t cleanedBytes = 0;
  if (!utf8CleanLookupWordToBuffer(text.view(), scratch, scratchCapacity, cleanedBytes)) {
    return DictionaryStatus::ReadError;
  }
  if (cleanedBytes == 0) return DictionaryStatus::NotFound;
  // NFC composition and boundary trimming cannot grow the input, so this
  // reuses DictionaryOwnedText's existing allocation rather than churning the
  // heap on page scanning, history, suggestions, or nested lookups.
  return text.assign(std::string_view(scratch, cleanedBytes)) ? DictionaryStatus::Found : DictionaryStatus::OutOfMemory;
}

DictionaryLookupFlowState DictionaryLookupFlow::stateForStatus(const DictionaryStatus status) {
  switch (status) {
    case DictionaryStatus::Found:
      return DictionaryLookupFlowState::Ready;
    case DictionaryStatus::NotFound:
      return DictionaryLookupFlowState::NotFound;
    case DictionaryStatus::Unavailable:
      return DictionaryLookupFlowState::Unavailable;
    case DictionaryStatus::ReadError:
      return DictionaryLookupFlowState::ReadError;
    case DictionaryStatus::OutOfMemory:
      return DictionaryLookupFlowState::OutOfMemory;
    case DictionaryStatus::Cancelled:
      return DictionaryLookupFlowState::Cancelled;
  }
  return DictionaryLookupFlowState::ReadError;
}

bool DictionaryLookupBackChain::push(const std::string_view word, const uint16_t definitionPage) {
  if (word.empty()) return false;
  DictionaryOwnedText retained;
  if (!retained.assign(word)) return false;
  if (depth_ == kCapacity) {
    for (uint8_t index = 1; index < depth_; ++index) entries_[index - 1] = std::move(entries_[index]);
    --depth_;
  }
  entries_[depth_].word = std::move(retained);
  entries_[depth_].definitionPage = definitionPage;
  ++depth_;
  return true;
}

bool DictionaryLookupBackChain::pop(DictionaryOwnedText& word, uint16_t& definitionPage) {
  if (depth_ == 0) return false;
  --depth_;
  word = std::move(entries_[depth_].word);
  definitionPage = entries_[depth_].definitionPage;
  entries_[depth_].definitionPage = 0;
  return true;
}

bool DictionaryLookupBackChain::top(std::string_view& word, uint16_t& definitionPage) const {
  if (depth_ == 0) return false;
  word = entries_[depth_ - 1].word.view();
  definitionPage = entries_[depth_ - 1].definitionPage;
  return true;
}

void DictionaryLookupBackChain::discardTop() {
  if (depth_ == 0) return;
  --depth_;
  entries_[depth_].word.reset();
  entries_[depth_].definitionPage = 0;
}

void DictionaryLookupBackChain::clear() {
  for (uint8_t index = 0; index < depth_; ++index) {
    entries_[index].word.reset();
    entries_[index].definitionPage = 0;
  }
  depth_ = 0;
}

void DictionaryLookupFlow::reset(const uint32_t openedAtMs) {
  state_ = DictionaryLookupFlowState::Idle;
  scanFailureState_ = DictionaryLookupFlowState::Idle;
  command_ = {};
  openedAtMs_ = openedAtMs;
  scanSliceStartedAtMs_ = openedAtMs;
  discoveredCount_ = 0;
  cursor_ = 0;
  replacementCursor_ = 0;
  definitionPage_ = 0;
  definitionPageCount_ = 0;
  directMode_ = false;
  scanComplete_ = true;
  scanFailed_ = false;
  hasSelection_ = false;
  waitingForNextCandidate_ = false;
  workerOwned_ = false;
  replacementPending_ = false;
  initialSelectionDeferred_ = false;
  exiting_ = false;
}

void DictionaryLookupFlow::beginPage(const uint32_t openedAtMs, const uint16_t discoveredCount, const bool scanComplete,
                                     const uint16_t restoredCursor, const bool deferInitialSelection) {
  reset(openedAtMs);
  scanComplete_ = scanComplete;
  discoveredCount_ = discoveredCount;
  initialSelectionDeferred_ = deferInitialSelection;
  state_ =
      scanComplete && discoveredCount == 0 ? DictionaryLookupFlowState::NotFound : DictionaryLookupFlowState::Loading;
  if (discoveredCount == 0) return;

  cursor_ = restoredCursor < discoveredCount ? restoredCursor : 0;
  if (initialSelectionDeferred_) return;
  hasSelection_ = true;
  startLookup(cursor_);
}

void DictionaryLookupFlow::beginDirect(const uint32_t openedAtMs) {
  reset(openedAtMs);
  directMode_ = true;
  hasSelection_ = true;
  state_ = DictionaryLookupFlowState::Loading;
  startLookup(0);
}

bool DictionaryLookupFlow::canStepScan(const uint32_t nowMs) const {
  return !directMode_ && !exiting_ && !scanComplete_ && !scanFailed_ && !workerOwned_ &&
         static_cast<uint32_t>(nowMs - scanSliceStartedAtMs_) < kScanSliceMs;
}

bool DictionaryLookupFlow::openDeadlineReached(const uint32_t nowMs) const {
  return static_cast<uint32_t>(nowMs - openedAtMs_) >= kOpenDeadlineMs;
}

bool DictionaryLookupFlow::initialBurstActive(const uint32_t nowMs) const {
  return !directMode_ && !scanComplete_ && !scanFailed_ && !openDeadlineReached(nowMs);
}

void DictionaryLookupFlow::queue(const DictionaryLookupFlowAction action, const uint16_t candidateIndex,
                                 const int definitionPage) {
  command_ = {action, generation_, candidateIndex, definitionPage};
}

DictionaryLookupFlowCommand DictionaryLookupFlow::takeCommand() {
  const DictionaryLookupFlowCommand command = command_;
  command_ = {};
  return command;
}

void DictionaryLookupFlow::startLookup(const uint16_t candidateIndex) {
  generation_ = nextGeneration(generation_);
  cursor_ = candidateIndex;
  definitionPage_ = 0;
  definitionPageCount_ = 0;
  waitingForNextCandidate_ = false;
  workerOwned_ = true;
  state_ = DictionaryLookupFlowState::Loading;
  queue(DictionaryLookupFlowAction::StartLookup, candidateIndex);
}

void DictionaryLookupFlow::startDefinitionCollection(const int page) {
  definitionPage_ = std::max(0, page);
  workerOwned_ = true;
  state_ = DictionaryLookupFlowState::Loading;
  queue(DictionaryLookupFlowAction::StartDefinitionCollection, cursor_, definitionPage_);
}

void DictionaryLookupFlow::replaceLookup(const uint16_t candidateIndex) {
  state_ = DictionaryLookupFlowState::Loading;
  definitionPage_ = 0;
  definitionPageCount_ = 0;
  if (workerOwned_) {
    replacementPending_ = true;
    replacementCursor_ = candidateIndex;
    queue(DictionaryLookupFlowAction::CancelAndJoin, cursor_);
    return;
  }
  startLookup(candidateIndex);
}

void DictionaryLookupFlow::onScanProgress(const uint16_t discoveredCount, const bool scanComplete,
                                          const DictionaryStatus status) {
  if (directMode_ || exiting_) return;
  const uint16_t previousCount = discoveredCount_;
  if (discoveredCount < previousCount) {
    scanComplete_ = false;
    scanFailed_ = true;
    scanFailureState_ = DictionaryLookupFlowState::ReadError;
    if (!workerOwned_) state_ = scanFailureState_;
    return;
  }
  discoveredCount_ = discoveredCount;
  scanComplete_ = scanComplete;

  if (status != DictionaryStatus::Found) {
    scanComplete_ = false;
    scanFailed_ = true;
    scanFailureState_ = stateForStatus(status);
    waitingForNextCandidate_ = false;
    if (!hasSelection_ && discoveredCount_ != 0 && !initialSelectionDeferred_ && !workerOwned_) {
      hasSelection_ = true;
      cursor_ = 0;
      startLookup(0);
      return;
    }
    if (!workerOwned_) state_ = scanFailureState_;
    return;
  }

  if (!hasSelection_ && discoveredCount_ != 0 && !initialSelectionDeferred_) {
    hasSelection_ = true;
    cursor_ = 0;
    startLookup(0);
    return;
  }

  if (waitingForNextCandidate_ && !workerOwned_) {
    waitingForNextCandidate_ = false;
    if (cursor_ + 1U < discoveredCount_) {
      replaceLookup(static_cast<uint16_t>(cursor_ + 1U));
    } else if (scanComplete_ && discoveredCount_ != 0) {
      replaceLookup(0);
    }
    return;
  }

  if (scanComplete_ && discoveredCount_ == 0 && !workerOwned_) state_ = DictionaryLookupFlowState::NotFound;
}

void DictionaryLookupFlow::onInitializationFailed(const DictionaryStatus status) {
  if (exiting_) return;
  command_ = {};
  scanComplete_ = true;
  scanFailed_ = true;
  scanFailureState_ = stateForStatus(status);
  waitingForNextCandidate_ = false;
  workerOwned_ = false;
  initialSelectionDeferred_ = false;
  state_ = stateForStatus(status);
}

bool DictionaryLookupFlow::selectInitialCandidate(const uint16_t candidateIndex) {
  if (exiting_ || directMode_ || !initialSelectionDeferred_ || hasSelection_ || workerOwned_ ||
      candidateIndex >= discoveredCount_) {
    return false;
  }
  initialSelectionDeferred_ = false;
  hasSelection_ = true;
  startLookup(candidateIndex);
  return true;
}

bool DictionaryLookupFlow::moveCursor(const int delta) {
  if (directMode_ || exiting_ || delta == 0 || discoveredCount_ == 0) return false;
  const int count = discoveredCount_;
  const int current = cursor_;
  int target = current + delta;

  if (!scanComplete_) {
    if (target < 0) target = 0;
    if (target >= count) {
      if (!scanFailed_) waitingForNextCandidate_ = true;
      return false;
    }
  } else {
    target %= count;
    if (target < 0) target += count;
  }

  if (target == current) return false;
  replaceLookup(static_cast<uint16_t>(target));
  return true;
}

bool DictionaryLookupFlow::replaceCurrentLookup() {
  if (exiting_ || !hasSelection_) return false;
  replaceLookup(cursor_);
  return true;
}

void DictionaryLookupFlow::onLookupFinished(const uint32_t generation, const DictionaryStatus status) {
  if (exiting_ || replacementPending_ || generation != generation_) return;
  workerOwned_ = false;
  if (status == DictionaryStatus::Found) {
    startDefinitionCollection(0);
  } else {
    state_ = status == DictionaryStatus::NotFound && scanFailed_ ? scanFailureState_ : stateForStatus(status);
  }
}

void DictionaryLookupFlow::onDefinitionEvent(const uint32_t generation, const DictionaryLookupFlowDefinitionEvent event,
                                             const int totalPages, const int publishedPage) {
  if (exiting_ || replacementPending_ || generation != generation_) return;
  workerOwned_ = false;
  switch (event) {
    case DictionaryLookupFlowDefinitionEvent::NeedsFontPrewarm:
      state_ = DictionaryLookupFlowState::Loading;
      queue(DictionaryLookupFlowAction::PrewarmDefinition, cursor_, definitionPage_);
      return;
    case DictionaryLookupFlowDefinitionEvent::Ready:
      if (totalPages <= 0 || publishedPage < 0 || publishedPage >= totalPages) {
        state_ = DictionaryLookupFlowState::ReadError;
        return;
      }
      definitionPageCount_ = totalPages;
      definitionPage_ = publishedPage;
      state_ = DictionaryLookupFlowState::Ready;
      return;
    case DictionaryLookupFlowDefinitionEvent::ReadError:
      state_ = DictionaryLookupFlowState::ReadError;
      return;
    case DictionaryLookupFlowDefinitionEvent::OutOfMemory:
      state_ = DictionaryLookupFlowState::OutOfMemory;
      return;
    case DictionaryLookupFlowDefinitionEvent::Cancelled:
      state_ = DictionaryLookupFlowState::Cancelled;
      return;
  }
}

void DictionaryLookupFlow::onPrewarmFinished(const uint32_t generation, const bool success) {
  if (exiting_ || replacementPending_ || generation != generation_) return;
  if (!success) {
    workerOwned_ = false;
    state_ = DictionaryLookupFlowState::OutOfMemory;
    return;
  }
  workerOwned_ = true;
  state_ = DictionaryLookupFlowState::Loading;
  queue(DictionaryLookupFlowAction::StartDefinitionLayout, cursor_, definitionPage_);
}

bool DictionaryLookupFlow::moveDefinitionPage(const int delta) {
  if (exiting_ || state_ != DictionaryLookupFlowState::Ready || definitionPageCount_ <= 0 || delta == 0) return false;
  const int64_t requested = static_cast<int64_t>(definitionPage_) + delta;
  const int page = static_cast<int>(std::clamp<int64_t>(requested, 0, definitionPageCount_ - 1));
  if (page == definitionPage_) return false;
  startDefinitionCollection(page);
  return true;
}

void DictionaryLookupFlow::onCommandFailed(const uint32_t generation, const DictionaryStatus status) {
  if (exiting_ || generation != generation_) return;
  workerOwned_ = false;
  state_ = stateForStatus(status);
}

void DictionaryLookupFlow::beginExit() {
  if (exiting_) return;
  exiting_ = true;
  state_ = DictionaryLookupFlowState::Cancelled;
  waitingForNextCandidate_ = false;
  replacementPending_ = false;
  if (workerOwned_) {
    queue(DictionaryLookupFlowAction::CancelAndJoin, cursor_);
  } else {
    queue(DictionaryLookupFlowAction::ReleaseResources, cursor_);
  }
}

void DictionaryLookupFlow::onWorkerReleased() {
  workerOwned_ = false;
  if (exiting_) {
    queue(DictionaryLookupFlowAction::ReleaseResources, cursor_);
    return;
  }
  if (!replacementPending_) return;
  replacementPending_ = false;
  startLookup(replacementCursor_);
}
