#include "BookStatsActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "BookStatsView.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/InputReleaseGuard.h"

namespace {

void drawPageIndicators(const GfxRenderer& renderer, const int currentPage, const int totalPages) {
  if (totalPages <= 1) {
    return;
  }

  constexpr int kDotSize = 8;
  constexpr int kDotSpacing = 6;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int totalDotWidth = totalPages * kDotSize + (totalPages - 1) * kDotSpacing;
  const int dotsStartX = (renderer.getScreenWidth() - totalDotWidth) / 2;
  const int dotY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
  for (int pageIndex = 0; pageIndex < totalPages; ++pageIndex) {
    const int dotX = dotsStartX + pageIndex * (kDotSize + kDotSpacing);
    if (pageIndex == currentPage) {
      renderer.fillRect(dotX, dotY, kDotSize, kDotSize, true);
    } else {
      renderer.drawRect(dotX, dotY, kDotSize, kDotSize, true);
    }
  }
}

}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const bool returnToHomeOnExit, const BookReadingStats* committedStats)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds),
      committedBookSeconds(committedStats ? committedStats->totalReadingSeconds : stats.totalReadingSeconds) {}

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const std::string& bookCachePath, const BookReadingStats& stats,
                                     const float progressPercent, const bool hasEstimatedTimeLeft,
                                     const uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                                     const GlobalReadingStats& allDevicesStats, const bool returnToHomeOnExit,
                                     const BookReadingStats* committedStats)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(title),
      bookCachePath(bookCachePath),
      stats(stats),
      globalStats(globalStats),
      allDevicesStats(allDevicesStats),
      showAllDevicesStats(true),
      returnToHomeOnExit(returnToHomeOnExit),
      progressPercent(progressPercent),
      hasEstimatedTimeLeft(hasEstimatedTimeLeft),
      estimatedTimeLeftSeconds(estimatedTimeLeftSeconds),
      committedBookSeconds(committedStats ? committedStats->totalReadingSeconds : stats.totalReadingSeconds) {}

void BookStatsActivity::refreshAllDevicesStats() {
  if (showAllDevicesStats) {
    allDevicesStats = GlobalReadingStats::loadAggregated(globalStats);
  }
}

bool BookStatsActivity::saveStats() {
  if (!edits.dirty || !hasEditableBook()) return true;
  BookReadingStats persisted = stats;
  persisted.totalReadingSeconds = committedBookSeconds;
  if (!edits.persist(bookCachePath, persisted, globalStats)) {
    LOG_ERR("STATS", "Date/completion edits remain unsaved");
    requestUpdate();
    return false;
  }
  refreshAllDevicesStats();
  edits.dirty = false;
  persistedChanges = true;
  setResult(ReadingStatsResult{true});
  return true;
}

void BookStatsActivity::beginDateEditing() {
  dateEditStatsSnapshot = stats;
  dateEditGlobalStatsSnapshot = globalStats;
  didChangeStatsBeforeDateEdit = edits.dirty;
  dateEditSnapshotValid = true;
  page = Page::EditDates;
  requestUpdate();
}

void BookStatsActivity::finishDateEditing(const bool saveChanges) {
  if (saveChanges || edits.saved.book || edits.saved.global) {
    if (!saveStats()) return;
  } else if (dateEditSnapshotValid) {
    stats = dateEditStatsSnapshot;
    globalStats = dateEditGlobalStatsSnapshot;
    edits.dirty = didChangeStatsBeforeDateEdit;
    setResult(ReadingStatsResult{persistedChanges});
  }

  dateEditSnapshotValid = false;
  page = Page::PerBook;
  requestUpdate();
}

void BookStatsActivity::cycleEditField() { selectedEditField = (selectedEditField + 1) % 6; }

ReadingStatsDate BookStatsActivity::defaultDateForField(const bool finishedField) const {
  if (finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }
  if (!finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (finishedField && stats.startDate.isValid()) {
    return stats.startDate;
  }
  if (!finishedField && stats.finishedDate.isValid()) {
    return stats.finishedDate;
  }

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return now.date;
  }
  return {2000, 1, 1};
}

void BookStatsActivity::applyCompletedState(const bool completed) {
  if (stats.isCompleted == completed) {
    return;
  }

  stats.isCompleted = completed;
  if (completed) {
    globalStats.completedBooks++;
    if (!stats.finishedDateManual && !stats.finishedDate.isValid()) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
}

void BookStatsActivity::normalizeEditedDates(const bool editedFinishedField) {
  if (!stats.startDate.isValid() || !stats.finishedDate.isValid()) {
    return;
  }
  if (compareReadingStatsDate(stats.finishedDate, stats.startDate) >= 0) {
    return;
  }

  if (editedFinishedField) {
    stats.startDate = stats.finishedDate;
  } else {
    stats.finishedDate = stats.startDate;
  }
}

void BookStatsActivity::clearEditedDate(const bool finishedField) {
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  date.clear();

  if (finishedField) {
    stats.finishedDateManual = false;
    applyCompletedState(false);
  } else {
    stats.startDateManual = false;
  }

  edits.changed();
  requestUpdate();
}

bool BookStatsActivity::shouldClearDateOnAdjust(const ReadingStatsDate& date, const bool finishedField,
                                                const int fieldIndex, const int delta) const {
  if (!date.isValid()) {
    return false;
  }

  switch (fieldIndex) {
    case 0:
      return (date.month == 1 && delta < 0) || (date.month == 12 && delta > 0);
    case 1: {
      const uint8_t monthDays = daysInMonth(date.year, date.month);
      return (date.day == 1 && delta < 0) || (date.day == monthDays && delta > 0);
    }
    case 2:
      return (date.year == 2000 && delta < 0) || (date.year == 2099 && delta > 0);
    default:
      return false;
  }
}

void BookStatsActivity::adjustSelectedDateField(const int delta) {
  const bool finishedField = selectedEditField >= 3;
  ReadingStatsDate& date = finishedField ? stats.finishedDate : stats.startDate;
  const int fieldIndex = selectedEditField % 3;

  if (shouldClearDateOnAdjust(date, finishedField, fieldIndex, delta)) {
    clearEditedDate(finishedField);
    return;
  }

  if (!date.isValid()) {
    date = defaultDateForField(finishedField);
  }

  switch (fieldIndex) {
    case 0: {
      int month = static_cast<int>(date.month) + delta;
      while (month < 1) {
        month += 12;
      }
      while (month > 12) {
        month -= 12;
      }
      date.month = static_cast<uint8_t>(month);
      break;
    }
    case 1: {
      const int monthDays = daysInMonth(date.year, date.month);
      int day = static_cast<int>(date.day) + delta;
      while (day < 1) {
        day += monthDays;
      }
      while (day > monthDays) {
        day -= monthDays;
      }
      date.day = static_cast<uint8_t>(day);
      break;
    }
    case 2: {
      int year = static_cast<int>(date.year) + delta;
      if (year < 2000) {
        year = 2099;
      } else if (year > 2099) {
        year = 2000;
      }
      date.year = static_cast<uint16_t>(year);
      break;
    }
  }

  const uint8_t monthDays = daysInMonth(date.year, date.month);
  if (date.day > monthDays) {
    date.day = monthDays;
  }

  if (finishedField) {
    stats.finishedDateManual = true;
    applyCompletedState(true);
  } else {
    stats.startDateManual = true;
  }
  normalizeEditedDates(finishedField);

  edits.changed();
  requestUpdate();
}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreInitialPowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  if (bookCachePath.empty()) page = Page::ThisDevice;
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  requestUpdate();
}

void BookStatsActivity::onExit() {
  saveStats();
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

bool BookStatsActivity::handleHomeGesture() {
  if (saveStats()) onGoHome();
  // Consume the gesture even after failure so the manager retains dirty edits.
  return true;
}

void BookStatsActivity::exitStatsActivity() {
  if (!saveStats()) return;
  if (returnToHomeOnExit) {
    onGoHome();
    return;
  }

  finish();
}

const ReadingLanguageTotals* BookStatsActivity::currentLanguages() const {
  if (page == Page::BookLanguages) return &stats.languageTotals;
  if (page == Page::DeviceLanguages) return &globalStats.languageTotals;
  if (page == Page::AllLanguages) return &allDevicesStats.languageTotals;
  return nullptr;
}
int BookStatsActivity::statsPageCount() const {
  return usesNoRtcSingleScreenLayout() ? (showAllDevicesStats ? 4 : 3) : (showAllDevicesStats ? 6 : 4);
}
BookStatsActivity::Page BookStatsActivity::statsPageAt(int index) const {
  static constexpr Page rtc[] = {Page::PerBook,         Page::BookLanguages, Page::ThisDevice,
                                 Page::DeviceLanguages, Page::AllDevices,    Page::AllLanguages};
  static constexpr Page noRtc[] = {Page::PerBook, Page::BookLanguages, Page::DeviceLanguages, Page::AllLanguages};
  return usesNoRtcSingleScreenLayout() ? noRtc[index] : rtc[index];
}
int BookStatsActivity::statsPageIndex() const {
  for (int i = 0; i < statsPageCount(); ++i)
    if (statsPageAt(i) == page) return i;
  return 0;
}
bool BookStatsActivity::showNextStatsPage() {
  const auto* languages = currentLanguages();
  if (languages && languageOffset + readingLanguageRowsPerPage(renderer) < readingLanguageRowCount(*languages)) {
    languageOffset += readingLanguageRowsPerPage(renderer);
    requestUpdate();
    return true;
  }
  const int index = statsPageIndex();
  if (index + 1 >= statsPageCount()) return false;
  page = statsPageAt(index + 1);
  languageOffset = 0;
  requestUpdate();
  return true;
}
bool BookStatsActivity::showPreviousStatsPage() {
  if (currentLanguages() && languageOffset > 0) {
    languageOffset = std::max(0, languageOffset - readingLanguageRowsPerPage(renderer));
    requestUpdate();
    return true;
  }
  const int index = statsPageIndex();
  if (!index) return false;
  page = statsPageAt(index - 1);
  languageOffset = 0;
  if (const auto* languages = currentLanguages()) {
    const int rows = readingLanguageRowsPerPage(renderer);
    languageOffset = std::max(0, readingLanguageRowCount(*languages) - 1) / rows * rows;
  }
  requestUpdate();
  return true;
}

bool BookStatsActivity::selectEditFieldFromTouchTarget(const int touchTarget) {
  if (touchTarget < BookStatsTouchTarget::DateFieldBase ||
      touchTarget >= BookStatsTouchTarget::DateFieldBase + BookStatsTouchTarget::DateFieldCount) {
    return false;
  }

  const int newEditField = touchTarget - BookStatsTouchTarget::DateFieldBase;
  if (selectedEditField != newEditField) {
    selectedEditField = newEditField;
    requestUpdate();
  }
  return true;
}

void BookStatsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power,
                                               ignoreInitialPowerRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer))) {
    if (page == Page::EditDates) {
      finishDateEditing(true);
    } else {
      exitStatsActivity();
    }
    return;
  }

  const bool upOrLeftPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                               mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool downOrRightPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                                  mappedInput.wasPressed(MappedInputManager::Button::Right);

  if (page == Page::EditDates) {
    int touchedTarget = -1;
    if (mappedInput.wasItemTouchedDown(touchedTarget) && selectEditFieldFromTouchTarget(touchedTarget)) {
      return;
    }
    int tappedTarget = -1;
    if (mappedInput.wasItemTapped(tappedTarget)) {
      if (selectEditFieldFromTouchTarget(tappedTarget)) {
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustUp) {
        adjustSelectedDateField(1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateAdjustDown) {
        adjustSelectedDateField(-1);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateSave) {
        finishDateEditing(true);
        return;
      }
      if (tappedTarget == BookStatsTouchTarget::DateCancel) {
        finishDateEditing(false);
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishDateEditing(true);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      cycleEditField();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustSelectedDateField(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustSelectedDateField(1);
      return;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    exitStatsActivity();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    if (page == Page::PerBook && hasEditableBook()) {
      beginDateEditing();
      return;
    }
    exitStatsActivity();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && showNextStatsPage()) {
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    if (!showPreviousStatsPage()) {
      exitStatsActivity();
    }
    return;
  }

  if (page == Page::PerBook) {
    int touchedTarget = -1;
    if (hasEditableBook() && mappedInput.wasItemTapped(touchedTarget) &&
        touchedTarget == BookStatsTouchTarget::StartedDaysStat) {
      beginDateEditing();
      return;
    }
    if (hasEditableBook() && upOrLeftPressed) {
      beginDateEditing();
      return;
    }
    if (downOrRightPressed) {
      showNextStatsPage();
      return;
    }
    return;
  }

  if (upOrLeftPressed) {
    showPreviousStatsPage();
    return;
  }

  if (downOrRightPressed) {
    showNextStatsPage();
  }
}

void BookStatsActivity::renderSaveFailure() const {
  if (!edits.dirty || !edits.failed) return;
  const auto header = TouchHeaderBackButton::compactHeaderRect(renderer);
  const int y = header.y + header.height + UITheme::getInstance().getMetrics().verticalSpacing;
  renderer.fillRect(0, y, renderer.getScreenWidth(), renderer.getLineHeight(UI_10_FONT_ID) + 8, false);
  renderer.drawCenteredText(UI_10_FONT_ID, y + 4, tr(STR_STATS_SAVE_FAILED));
}

void BookStatsActivity::render(RenderLock&&) {
  if (const auto* languages = currentLanguages()) {
    const char* scope = page == Page::BookLanguages     ? bookTitle.c_str()
                        : page == Page::DeviceLanguages ? tr(STR_STATS_THIS_DEVICE_SCREEN)
                                                        : tr(STR_STATS_ALL_DEVICES_SCREEN);
    renderReadingLanguagesPage(renderer, &mappedInput, scope, *languages, languageOffset,
                               statsPageIndex() + 1 < statsPageCount());
    drawPageIndicators(renderer, statsPageIndex(), statsPageCount());
    renderSaveFailure();
    renderer.displayBuffer();
    return;
  }
  if (usesNoRtcSingleScreenLayout() && page == Page::PerBook) {
    renderNoRtcCombinedStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                                 estimatedTimeLeftSeconds, globalStats,
                                 showAllDevicesStats ? &allDevicesStats : nullptr, true);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", tr(STR_MORE));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    drawPageIndicators(renderer, statsPageIndex(), statsPageCount());
    renderSaveFailure();
    renderer.displayBuffer();
    return;
  }

  switch (page) {
    case Page::PerBook:
      renderPerBookStatsPage(renderer, &mappedInput, bookTitle, stats, progressPercent, hasEstimatedTimeLeft,
                             estimatedTimeLeftSeconds, true, hasEditableBook(), true);
      break;
    case Page::ThisDevice:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_THIS_DEVICE_SCREEN), globalStats, true, true);
      break;
    case Page::AllDevices:
      renderGlobalStatsPage(renderer, &mappedInput, tr(STR_STATS_ALL_DEVICES_SCREEN), allDevicesStats, true, true);
      break;
    case Page::BookLanguages:
    case Page::DeviceLanguages:
    case Page::AllLanguages:
      break;
    case Page::EditDates:
      renderEditBookDatesPage(renderer, &mappedInput, bookTitle, stats, selectedEditField, true);
      break;
  }
  if (page != Page::EditDates) {
    drawPageIndicators(renderer, statsPageIndex(), statsPageCount());
  }
  renderSaveFailure();
  renderer.displayBuffer();
}
