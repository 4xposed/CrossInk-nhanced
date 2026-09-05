#pragma once

#include <Epub/Page.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "DictionaryDefinitionModel.h"
#include "DictionaryLookupFlow.h"
#include "EpubLookupRequest.h"
#include "PageTextSource.h"
#include "PageWordScanCache.h"
#include "PageWordScanner.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/DictionaryEngine.h"

// The request's callback/context pair remains caller-owned and must stay valid
// until onExit() returns. The borrowed dictionary font name is copied into this
// activity during construction.
class EpubReaderWordLookupActivity final : public Activity {
 public:
  EpubReaderWordLookupActivity(GfxRenderer&, MappedInputManager&, std::unique_ptr<Page>, EpubLookupPageRequest);
  EpubReaderWordLookupActivity(GfxRenderer&, MappedInputManager&, std::string directWord, EpubLookupPageRequest);
  ~EpubReaderWordLookupActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  bool blocksGlobalInput() const override { return true; }

 private:
  enum class WorkerJobKind : uint8_t { None, Lookup, CollectDefinition, LayoutDefinition };

  struct RenderSnapshot {
    DictionaryLookupFlowState state = DictionaryLookupFlowState::Idle;
    DictionaryBackendKind backend = DictionaryBackendKind::StarDict;
    PageTextBounds highlight{};
    uint16_t cursor = 0;
    uint16_t discoveredCount = 0;
    int definitionPage = 0;
    int definitionPageCount = 0;
    bool scanComplete = true;
    bool selectionValid = false;
    bool highlightValid = false;
    PageTextBounds definitionHighlight{};
    bool definitionHighlightValid = false;
  };

  struct PanelLayout {
    Rect panel{};
    int contentX = 0;
    int contentWidth = 0;
    int titleY = 0;
    int bodyY = 0;
    int bodyBottom = 0;
    int lineHeight = 1;
    int linesPerPage = 1;
  };

  struct DefinitionToken {
    std::string_view text;
    PageTextBounds bounds{};
    uint16_t index = 0;
    bool valid = false;
  };

  static constexpr size_t kDictionaryFontFamilyBytes = 64;
  static constexpr size_t kScanCachePathBytes = 528;
  static constexpr size_t kLookupTextBytes = 256;
  static constexpr int kCleanupRefreshInterval = 10;
  static constexpr uint32_t kLongPressMs = 600;

  void initializeRequest(EpubLookupPageRequest&& request);
  DictionaryStatus openRoutedEngine();
  DictionaryStatus openEngine();
  DictionaryStatus initializePageMode(bool deferInitialSelection = false);
  void initializeDirectMode();
  void shutdownBeforeFinish();
  void releaseOwnedState(bool renderLockAlreadyHeld);
  void saveCompleteScanCache();
  void executeFlowCommands();
  DictionaryStatus startWorker(WorkerJobKind job, uint32_t generation);
  static void runWorkerJob(void* context);
  void runWorker();
  void processWorkerCompletion();
  void processDefinitionCompletion(uint32_t generation);
  void runScanSlice();

  const PageWordCandidate* selectedCandidate() const;
  const PageWordCandidate* candidateAt(uint16_t index) const;
  DictionaryStatus prepareLookupText(uint16_t candidateIndex);
  DictionaryStatus encodeCandidateText(const PageWordCandidate& candidate, DictionaryOwnedText& out);
  void publishRenderSnapshot(bool requestRender = true);
  void publishLoadingBeforeReplacement();
  void updateHighlightSnapshot(RenderSnapshot& snapshot) const;
  void logReadyTime();
  void observeOpenDeadline(uint32_t nowMs);
  bool resolvePendingInitialTouch();
  uint16_t candidateAtPoint(int x, int y, bool exactOnly) const;

  void clearDefinitionSelection();
  bool enterDefinitionSelection();
#if CROSSINK_APP_CAP_TOUCH
  bool selectDefinitionTokenAt(int x, int y, bool beginTouchDrag);
#endif
  bool moveDefinitionSelection(int delta);
  bool lookupDefinitionSelection();
  bool lookupDefinitionSelectionRange();
#if CROSSINK_APP_CAP_TOUCH
  bool lookupDefinitionTokenAt(int x, int y);
#endif
  bool findDefinitionToken(uint16_t wantedIndex, int touchX, int touchY, bool useTouch, DefinitionToken& selected,
                           uint16_t& tokenCount) const;
  int definitionFontId(bool isIpa) const;
  int measureDefinitionText(std::string_view text, EpdFontFamily::Style style, bool isIpa = false) const;

  PanelLayout panelLayoutLocked() const;
  void renderReaderBackground();
  void drawPanelFrame(const PanelLayout& layout) const;
  void drawPanelHeader(const PanelLayout& layout, const RenderSnapshot& snapshot) const;
  void drawLoadingOrError(const PanelLayout& layout, DictionaryLookupFlowState state) const;
  void drawDefinition(const PanelLayout& layout) const;
  void drawDefinitionPass(const PanelLayout& layout) const;
  void drawButtonHints() const;
  void displayPanelRefresh(bool framebufferContainedPage);

  void finishLookup(bool cancelled);
  void openDictionarySwitcher();
  void openSuggestions();
  bool restartCurrentLookup(std::string_view word, bool suggestion, bool recordHistory = true,
                            bool pushDefinitionBack = false);
  bool returnToPreviousDefinition();
  void resetDefinitionBackChain();
  void returnCurrentClipping();
#if CROSSINK_APP_CAP_TOUCH
  bool panelContainsLocked(int x, int y) const;
#endif
  uint16_t nearestCandidateAt(int x, int y) const;
  DictionaryStatus reopenCancelledEngine();
  DictionaryLookupFlowDefinitionEvent definitionEvent(DefinitionBuildState state) const;

  std::unique_ptr<Page> page_;
  std::string directWord_;
  std::string bookLanguage_;
  std::string bookCachePath_;
  std::array<char, kDictionaryFontFamilyBytes> dictionaryFontFamilyName_{};
  std::array<char, kScanCachePathBytes> scanCachePath_{};
  std::array<char, kLookupTextBytes> lookupDisplayPrefix_{};
  uint16_t spineIndex_ = 0;
  uint16_t pageIndex_ = 0;
  int marginLeft_ = 0;
  int marginTop_ = 0;
  int reservedBottomHeight_ = 0;
  int initialTouchX_ = -1;
  int initialTouchY_ = -1;
  uint8_t dictionaryFontPointSize_ = 0;
  void* readerContext_ = nullptr;
  void (*readerBackgroundRender_)(void*) = nullptr;
  std::unique_ptr<Page> (*readerPageReload_)(void*) = nullptr;

  DictionaryEngine engine_;
  DictionaryCapabilities capabilities_{};
  DictionaryBackendKind cacheBackend_ = DictionaryBackendKind::StarDict;
  uint64_t cacheDictionarySignature_ = 0;
  HorizontalPageTextSource pageSource_;
  PageWordScanner scanner_;
  PageWordScanCache scanCache_;
  DictionaryDefinitionModel definitionModel_;
  DictionaryLookupFlow flow_;
  DictionaryLookupBackChain definitionBackChain_;
  DictionaryResult activeResult_;
  DictionaryResult pendingResult_;
  DictionaryOwnedText lookupText_;
  DictionaryOwnedText replacementLookupText_;
  DictionaryOwnedText dictionaryOverridePath_;
  DictionaryOwnedText dictionarySwitchWord_;
  DictionaryOwnedText bookReading_;
  DictionarySuggestions pendingSuggestions_;

  RenderSnapshot renderSnapshot_{};
  WorkerJobKind workerJob_ = WorkerJobKind::None;
  uint32_t workerGeneration_ = 0;
  std::atomic<uint32_t> completedGeneration_{0};
  std::atomic<uint8_t> completedJob_{static_cast<uint8_t>(WorkerJobKind::None)};
  std::atomic<DictionaryStatus> completedStatus_{DictionaryStatus::Unavailable};

  uint32_t openedAtMs_ = 0;
  int definitionFontId_ = 0;
  int fastRefreshCount_ = 0;
  int pendingDefinitionPageRestore_ = -1;
  uint16_t definitionTokenIndex_ = 0;
  uint16_t definitionTokenCount_ = 0;
  uint16_t definitionSelectionAnchor_ = 0;
  uint16_t lookupDisplayPrefixLength_ = 0;
  const char* syntheticNameDefinition_ = nullptr;
  bool pageMode_ = false;
  bool engineOpen_ = false;
  bool cacheIdentityValid_ = false;
  bool cacheLoaded_ = false;
  bool exiting_ = false;
  bool shutdownComplete_ = false;
  bool renderDisabled_ = false;
  bool initialRender_ = true;
  bool framebufferContainsPage_ = false;
  bool autoLookupInitialWord_ = false;
  bool pendingInitialTouchSelection_ = false;
  bool dismissOnInitialTouchMiss_ = false;
  bool nearestOnInitialTouchMiss_ = false;
  bool initialTouchMiss_ = false;
  bool openDeadlineLogged_ = false;
  bool definitionSelectionMode_ = false;
  bool definitionMultiSelectMode_ = false;
  bool definitionConfirmReleaseConsumed_ = false;
  bool definitionTouchDragLookup_ = false;
  bool dictionarySwitchHeld_ = false;
  bool dictionaryFontActive_ = false;
  bool readyTimeLogged_ = false;
  bool ignoreInitialBackRelease_ = false;
  bool exitAllOnBackRelease_ = false;
  bool lookupWasSuggestion_ = false;
  bool initialRecordLookupHistory_ = true;
  bool recordLookupHistory_ = true;
  bool notFoundShouldRecordHistory_ = true;
  bool notFoundHistoryRecorded_ = false;
  bool suggestionsShownForGeneration_ = false;
  bool lookupSyntheticKatakanaName_ = false;
};
