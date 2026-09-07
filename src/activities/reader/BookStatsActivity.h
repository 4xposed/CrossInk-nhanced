#pragma once

#include <HalClock.h>

#include <string>

#include "../Activity.h"
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsSave.h"

class BookStatsActivity final : public Activity {
  enum class Page : uint8_t {
    PerBook,
    BookLanguages,
    ThisDevice,
    DeviceLanguages,
    AllDevices,
    AllLanguages,
    EditDates
  };

  std::string bookTitle;
  std::string bookCachePath;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  GlobalReadingStats allDevicesStats;
  bool showAllDevicesStats = false;
  bool returnToHomeOnExit = false;
  float progressPercent = -1.0f;
  bool hasEstimatedTimeLeft = false;
  uint32_t estimatedTimeLeftSeconds = 0;
  uint32_t committedBookSeconds = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  Page page = Page::PerBook;
  int selectedEditField = 0;
  ReadingStatsEditState edits;
  bool persistedChanges = false;
  int languageOffset = 0;
  const ReadingLanguageTotals* currentLanguages() const;
  int statsPageIndex() const;
  int statsPageCount() const;
  Page statsPageAt(int index) const;
  BookReadingStats dateEditStatsSnapshot;
  GlobalReadingStats dateEditGlobalStatsSnapshot;
  bool dateEditSnapshotValid = false;
  bool didChangeStatsBeforeDateEdit = false;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;

  bool hasEditableBook() const { return !bookCachePath.empty() && halClock.isAvailable(); }
  bool usesNoRtcSingleScreenLayout() const { return !halClock.isAvailable(); }
  void refreshAllDevicesStats();
  bool saveStats();
  void renderSaveFailure() const;
  void beginDateEditing();
  void finishDateEditing(bool saveChanges);
  void cycleEditField();
  void adjustSelectedDateField(int delta);
  void applyCompletedState(bool completed);
  ReadingStatsDate defaultDateForField(bool finishedField) const;
  void clearEditedDate(bool finishedField);
  bool shouldClearDateOnAdjust(const ReadingStatsDate& date, bool finishedField, int fieldIndex, int delta) const;
  void normalizeEditedDates(const bool editedFinishedField);
  void exitStatsActivity();
  bool showPreviousStatsPage();
  bool showNextStatsPage();
  bool selectEditFieldFromTouchTarget(int touchTarget);

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    bool returnToHomeOnExit = false, const BookReadingStats* committedStats = nullptr);
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    const GlobalReadingStats& allDevicesStats, bool returnToHomeOnExit = false,
                    const BookReadingStats* committedStats = nullptr);

  void onEnter() override;
  void onExit() override;
  bool handleHomeGesture() override;
  bool prepareToSuspend() override { return saveStats(); }
  bool cancelSuspensionOnFailure() const override { return true; }
  bool preventAutoSleep() override { return edits.dirty && edits.failed; }
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
