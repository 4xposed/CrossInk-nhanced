#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/DictionaryEngineTypes.h"

enum class DictionaryLookupFlowState : uint8_t {
  Idle,
  Loading,
  Ready,
  NotFound,
  Unavailable,
  ReadError,
  OutOfMemory,
  Cancelled,
};

enum class DictionaryLookupPanelRefresh : uint8_t { Full, Fast };

constexpr bool dictionaryLookupShouldRenderSnapshot(const DictionaryLookupFlowState state,
                                                    const bool initialBurstActive) {
  return state != DictionaryLookupFlowState::Loading || !initialBurstActive;
}

constexpr DictionaryLookupPanelRefresh dictionaryLookupInitialPanelRefresh(const bool framebufferContainsPage,
                                                                           const bool pageMode = false) {
  // Menu entry redraws the page in RAM before displaying the panel. It needs
  // the same fast transition as a lookup opened directly from the reader.
  return framebufferContainsPage || pageMode ? DictionaryLookupPanelRefresh::Fast : DictionaryLookupPanelRefresh::Full;
}

enum class DictionaryLookupFlowAction : uint8_t {
  None,
  StartLookup,
  StartDefinitionCollection,
  PrewarmDefinition,
  StartDefinitionLayout,
  CancelAndJoin,
  ReleaseResources,
};

enum class DictionaryLookupFlowDefinitionEvent : uint8_t {
  NeedsFontPrewarm,
  Ready,
  ReadError,
  OutOfMemory,
  Cancelled,
};

struct DictionaryLookupFlowCommand {
  DictionaryLookupFlowAction action = DictionaryLookupFlowAction::None;
  uint32_t generation = 0;
  uint16_t candidateIndex = 0;
  int definitionPage = 0;
};

enum class DictionaryLookupHistoryKind : uint8_t { Direct, Stem, AltForm, Suggestion };

DictionaryLookupHistoryKind dictionaryLookupHistoryKind(const DictionaryResult& result, bool suggestion);
DictionaryStatus normalizeRetainedDictionaryLookupText(DictionaryBackendKind backend, DictionaryOwnedText& text,
                                                       char* scratch, size_t scratchCapacity);

constexpr bool dictionaryLookupUsesSideButtons(const uint8_t preference, const uint8_t sideButtonLayout,
                                               const uint8_t disabledSideButtonLayout) {
  return preference != 0 && sideButtonLayout != disabledSideButtonLayout;
}

constexpr bool dictionaryLookupPowerReleaseDismisses(const uint8_t configuredShortPowerAction,
                                                     const uint8_t lookupWordAction, const bool powerReleased,
                                                     const bool downReleased) {
  return configuredShortPowerAction == lookupWordAction && powerReleased && !downReleased;
}

enum class DictionaryLookupNavigationButton : uint8_t { Left, Right, Up, Down };

struct DictionaryLookupScrollButtons {
  DictionaryLookupNavigationButton up = DictionaryLookupNavigationButton::Up;
  DictionaryLookupNavigationButton down = DictionaryLookupNavigationButton::Down;
};

constexpr DictionaryLookupScrollButtons dictionaryLookupScrollButtons(const bool sideButtonsForLookup,
                                                                      const bool frontNavigationSwapped) {
  if (!sideButtonsForLookup) return {};
  return frontNavigationSwapped ? DictionaryLookupScrollButtons{DictionaryLookupNavigationButton::Right,
                                                                DictionaryLookupNavigationButton::Left}
                                : DictionaryLookupScrollButtons{DictionaryLookupNavigationButton::Left,
                                                                DictionaryLookupNavigationButton::Right};
}

constexpr bool dictionaryLookupInitialTouchMissIsConclusive(const bool exactAutoLookup, const bool touchesGlyph,
                                                            const bool touchedGlyphProcessed, const bool scanComplete) {
  if (scanComplete) return true;
  if (!exactAutoLookup) return false;
  return !touchesGlyph || touchedGlyphProcessed;
}

struct DictionaryLookupCandidatePresentation {
  uint16_t cursor = 0;
  uint16_t discoveredCount = 0;
  bool selectionValid = false;
  bool showPositionHeader = false;
};

constexpr DictionaryLookupCandidatePresentation dictionaryLookupCandidatePresentation(const bool pageMode,
                                                                                      const bool hasSelection,
                                                                                      const uint16_t cursor,
                                                                                      const uint16_t discoveredCount) {
  const bool selectionValid = pageMode && hasSelection && discoveredCount != 0 && cursor < discoveredCount;
  return {cursor, discoveredCount, selectionValid, selectionValid};
}

// Main-task-only orchestration for the floating lookup activity. It owns no
// renderer, filesystem, wall clock, or worker implementation, so every state
// transition is deterministic in native tests. Worker callbacks publish their
// result separately; the activity feeds completion back on the main task.
class DictionaryLookupFlow {
 public:
  static constexpr uint32_t kScanSliceMs = 50;
  static constexpr uint32_t kOpenDeadlineMs = 1500;

  void beginPage(uint32_t openedAtMs, uint16_t discoveredCount, bool scanComplete, uint16_t restoredCursor,
                 bool deferInitialSelection = false);
  void beginDirect(uint32_t openedAtMs);

  void beginScanSlice(uint32_t nowMs) { scanSliceStartedAtMs_ = nowMs; }
  bool canStepScan(uint32_t nowMs) const;
  bool openDeadlineReached(uint32_t nowMs) const;
  bool initialBurstActive(uint32_t nowMs) const;
  void onScanProgress(uint16_t discoveredCount, bool scanComplete, DictionaryStatus status);
  void onInitializationFailed(DictionaryStatus status);
  bool selectInitialCandidate(uint16_t candidateIndex);

  bool moveCursor(int delta);
  bool replaceCurrentLookup();
  bool moveDefinitionPage(int delta);

  void onLookupFinished(uint32_t generation, DictionaryStatus status);
  void onDefinitionEvent(uint32_t generation, DictionaryLookupFlowDefinitionEvent event, int totalPages = 0,
                         int publishedPage = 0);
  void onPrewarmFinished(uint32_t generation, bool success);
  void onCommandFailed(uint32_t generation, DictionaryStatus status);

  void beginExit();
  void onWorkerReleased();

  DictionaryLookupFlowCommand takeCommand();

  DictionaryLookupFlowState state() const { return state_; }
  uint32_t generation() const { return generation_; }
  uint16_t cursor() const { return cursor_; }
  uint16_t discoveredCount() const { return discoveredCount_; }
  bool scanComplete() const { return scanComplete_; }
  bool scanFailed() const { return scanFailed_; }
  bool waitingForNextCandidate() const { return waitingForNextCandidate_; }
  bool directMode() const { return directMode_; }
  bool initialSelectionDeferred() const { return initialSelectionDeferred_; }
  bool hasSelection() const { return hasSelection_; }
  bool canStillProduceResult() const {
    return !exiting_ &&
           (state_ == DictionaryLookupFlowState::Loading || (!directMode_ && !scanComplete_ && !scanFailed_));
  }
  bool showPositionHeader() const {
    return dictionaryLookupCandidatePresentation(!directMode_, hasSelection_, cursor_, discoveredCount_)
        .showPositionHeader;
  }
  bool workerOwned() const { return workerOwned_; }
  bool exiting() const { return exiting_; }
  int definitionPage() const { return definitionPage_; }
  int definitionPageCount() const { return definitionPageCount_; }

 private:
  static DictionaryLookupFlowState stateForStatus(DictionaryStatus status);
  void reset(uint32_t openedAtMs);
  void queue(DictionaryLookupFlowAction action, uint16_t candidateIndex = 0, int definitionPage = 0);
  void startLookup(uint16_t candidateIndex);
  void startDefinitionCollection(int page);
  void replaceLookup(uint16_t candidateIndex);

  DictionaryLookupFlowState state_ = DictionaryLookupFlowState::Idle;
  DictionaryLookupFlowState scanFailureState_ = DictionaryLookupFlowState::Idle;
  DictionaryLookupFlowCommand command_{};
  uint32_t openedAtMs_ = 0;
  uint32_t scanSliceStartedAtMs_ = 0;
  uint32_t generation_ = 0;
  uint16_t discoveredCount_ = 0;
  uint16_t cursor_ = 0;
  uint16_t replacementCursor_ = 0;
  int definitionPage_ = 0;
  int definitionPageCount_ = 0;
  bool directMode_ = false;
  bool scanComplete_ = true;
  bool scanFailed_ = false;
  bool hasSelection_ = false;
  bool waitingForNextCandidate_ = false;
  bool workerOwned_ = false;
  bool replacementPending_ = false;
  bool initialSelectionDeferred_ = false;
  bool exiting_ = false;
};

static_assert(sizeof(DictionaryLookupFlow) <= 64, "Lookup orchestration must remain a small activity member");

// Bounded, fallible history for lookup-inside-definition navigation. It owns
// only the query needed to rebuild each prior immutable definition page; no
// definition text/model is retained. The 50-entry cap matches the existing
// visible lookup-history contract without growable container allocations.
class DictionaryLookupBackChain {
 public:
  static constexpr uint8_t kCapacity = 50;

  bool push(std::string_view word, uint16_t definitionPage);
  bool top(std::string_view& word, uint16_t& definitionPage) const;
  void discardTop();
  bool pop(DictionaryOwnedText& word, uint16_t& definitionPage);
  void clear();
  uint8_t depth() const { return depth_; }

 private:
  struct Entry {
    DictionaryOwnedText word;
    uint16_t definitionPage = 0;
  };

  std::array<Entry, kCapacity> entries_{};
  uint8_t depth_ = 0;
};
