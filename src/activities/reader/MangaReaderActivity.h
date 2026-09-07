#pragma once

#include <MangaBook.h>
#include <MangaImageGeometry.h>
#include <MangaPendingInput.h>
#include <MangaPixelCache.h>

#include "BookReadingStats.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "GlobalReadingStats.h"
#include "MangaMenuState.h"
#include "MangaNavigation.h"
#include "MangaPageTextSource.h"
#include "MangaPrefetch.h"
#include "MangaProgressStore.h"
#include "MangaStatsCommit.h"
#include "ReaderProgressSaveDebouncer.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Foreground owner with one isolated speculative cache worker. MangaBook's borrowed page bytes and reusable path buffer
// are accessed under RenderLock; no file or decoder survives a render operation.
class MangaReaderActivity final : public Activity {
 public:
  MangaReaderActivity(GfxRenderer& renderer, MappedInputManager& input, std::string folder);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool prepareToSuspend() override;
  bool cancelSuspensionOnFailure() const override { return suspensionPersistenceFailed; }
  void onResume() override;
  void render(RenderLock&&) override;
#ifdef SIMULATOR
  manga::Position simulatorPosition();
  bool simulatorJumpWhenIdle(manga::Position target);
  bool simulatorQueueCurrentSource();
  bool simulatorStartWarmWhenIdle();
  bool simulatorPendingRender();
  bool simulatorMenuActive();
  bool simulatorMenuOptionCenter(int index, int& x, int& y);
  bool simulatorNoOcrFeedback();
  void simulatorFailNextBwRestore();
  bool simulatorFeedbackIs(StrId message);
  unsigned simulatorRenderErrors() const { return renderErrorsForTest; }
#endif
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool usesFullScreenReaderVerticalSwipes() const override { return !menu.isActive(); }
  bool blocksGlobalInput() const override { return menu.isActive(); }
  bool allowPowerAsConfirmInReaderMode() const override { return menu.isActive(); }
  bool openReaderSettingsMenu() override;
  bool handleShortcutAction(uint8_t action) override;
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  bool preventAutoSleep() override { return autoTurn.active() || statsCommit.failed() || suspensionPersistenceFailed; }
  ScreenshotInfo getScreenshotInfo() const override;
  void onInputLockChanged(bool locked) override;
  bool handleTwoFingerRotation(bool clockwise) override;
  bool prepareManualRefresh() override;
  std::string getCurrentBookPath() const override { return folder; }

 private:
  std::unique_ptr<manga::MangaPrefetch> prefetch;
  manga::ImageViewports viewports;
  unsigned long renderedAtMs = 0, prefetchRetryAtMs = 0;
  uint8_t prefetchCandidate = 0;
  manga::PendingInput pendingInput;
  bool pendingBack = false, pendingRender = false, suspended = false;
  bool foregroundDraining = false;
  bool pendingLookup = false;
  bool inputLocked = false, childActive = false;
  bool pendingScreenshot = false, pendingCacheDelete = false, leaveAfterMessage = false;
  bool incrementalStats = false, suspensionPersistenceFailed = false;
  StrId pendingFeedback = StrId::STR_NONE_OPT;
  manga::AutoTurn autoTurn;
  manga::StatsCommit statsCommit;
  bool foregroundReadyLocked();
  void pollPrefetchLocked();
  void warmLocked();
  void captureViewportsLocked();
  std::string folder;
  manga::MangaBook book;
  manga::MangaProgressStore progressStore;
  manga::Progress progress;
  manga::Position position;
  manga::PageAvailability available;
  manga::format::PageView page;
  // Paths scale with the folder and are allocated once on entry, never on stack.
  std::unique_ptr<char[]> path;
  size_t pathCapacity = 0;
  std::string imagePath;
  // One lifetime scratch area serves BMP source rows and packed cache replay.
  // It is not a second framebuffer; allocation failure retains BW rendering.
  std::unique_ptr<uint8_t[]> pixelScratch;
  manga::MangaPixelCache pixelCache;
  manga::ImageGeometry imageGeometry;
  RenderConfig imageConfig{};
  GfxRenderer::Orientation imageOrientation = GfxRenderer::Portrait;
  bool pixelsReady = false;
#ifdef SIMULATOR
  bool failNextBwRestoreForTest = false;
  unsigned renderErrorsForTest = 0;
#endif
  std::string statsCachePath;
  ReaderProgressSaveDebouncer saveDebouncer;
  unsigned long pageShownAtMs = 0UL;
  uint32_t sessionReadingMs = 0;
  uint32_t physicalPageReadingMs = 0;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  OptionPopup menu;
  manga::MenuAction pendingMenuAction = manga::MenuAction::None;
  int refreshCountdown = 0;
  unsigned long lastSaveFailureMs = 0;
  bool saveFailed = false;
  bool ready = false;
  bool finalPageCounted = false;
  bool imageDirty = true;
  GfxRenderer::Orientation entryOrientation = GfxRenderer::Portrait;

  bool loadPageLocked(uint32_t number);
  void move(bool forward);
  void moveLocked(bool forward);
  void applyMoveLocked(const manga::Move& target, bool forward);
  void showMenuLocked();
  void jump(uint32_t number, int16_t panel = -1);
  bool saveProgressLocked();
  void observeProgressLocked();
  bool drawImageLocked();
  bool drawCachedPixelsLocked();
  bool displayImageGrayscaleLocked();
  void handleMenuAction(manga::MenuAction action);
  void openMangaSettings();
  void openAutoTurnMenu();
  void openQr();
  void confirmCacheDelete();
  void deleteCacheWhenReady();
  bool queueShortcut(manga::MenuAction action);
  void drawStatusLocked(bool grayMask = false);
  void openLookup();
  void openTranslation();
  void openLookupHistory();
  void lookupBackgroundLocked(PageTextSourceView source);
  void showLookupMessage(const char* message);
  void childReturned();
  MangaLookupGeometry lookupGeometry;
  void toggleBookmark();
  void showSelection(bool bookmarks);
  void pauseReadingStatsTimer();
  void resumeReadingStatsTimer();
  bool elapsedPageReadingMs(uint32_t& seconds) const;
  void recordCurrentPageReadingTime();
  void recordForwardPageTurn(uint32_t seconds);
  bool commitReadingStats();
  bool saveDurableStateLocked();
  void setBookCompleted(bool completed);
};
