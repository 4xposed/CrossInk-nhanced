#include "HomeActivity.h"

#include <Epub.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <optional>

#include "BookPreview.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "HomeNavigation.h"
#include "KOReaderCredentialStore.h"
#include "MangaCoverInput.h"
#include "QuickActions.h"
#include "RecentBookProgress.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/TouchUi.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "components/icons/brightnessMenuIcon.h"
void HomeActivity::onEnter() {
  NavigationListActivity::onEnter();
  RECENT_BOOKS.ensureLoaded();
  const auto& recent = RECENT_BOOKS.getBooks();
  // Prefer the actual last-opened book; a stale path must not hide navigation.
  const std::string path = initialBookPath.empty() ? APP_STATE.openEpubPath : initialBookPath;
  for (const auto& item : recent)
    if (item.path == path && Storage.exists(item.path.c_str())) {
      book = item;
      break;
    }
  if (book.path.empty())
    for (const auto& item : recent)
      if (Storage.exists(item.path.c_str())) {
        book = item;
        break;
      }
  if (book.path.empty() && !path.empty() && Storage.exists(path.c_str())) {
    book.path = path;
    book.title = path.substr(path.find_last_of('/') + 1);
  }
  selected = firstSelection();
  switch (initialMenu) {
    case HomeMenuItem::LIBRARY:
      selected = 0;
      break;
    case HomeMenuItem::ANKI:
      selected = 1;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      selected = 2;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      selected = 3;
      break;
    case HomeMenuItem::TOOLS:
    case HomeMenuItem::FILE_BROWSER:
    case HomeMenuItem::RECENTS:
      selected = 4;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      selected = 5;
      break;
    default:
      break;
  }
  if (TouchUi::enabled(mappedInput)) {
    // Home must not reopen an EPUB parser just to fill in an optional percentage.
    if (!book.path.empty())
      readingPercent = FsHelpers::hasEpubExtension(book.path) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                              : RecentBookProgress::loadPercent(book);
    touchMenuOpen = selected >= 2;
    touchButtonFocus = initialMenu != HomeMenuItem::NONE;
  }
  coverWork.authorizeIntent();
  requestUpdate();
}
void HomeActivity::onExit() { Activity::onExit(); }
const char* HomeActivity::itemLabel(int index) const {
  static constexpr StrId labels[] = {StrId::STR_LIBRARY,        StrId::STR_ANKI,  StrId::STR_OPDS_BROWSER,
                                     StrId::STR_TRANSFER_FILES, StrId::STR_TOOLS, StrId::STR_SETTINGS_TITLE};
  return I18N.get(labels[index]);
}
freeink::ui::BitmapRef HomeActivity::itemIcon(const int index) const {
  if (index >= 0 && index < itemCount() && kHomeDestinations[index] == HomeDestination::Anki) {
    return freeink::ui::bitmapFromIcon(icon_anki_24);
  }
  return {};
}
void HomeActivity::activate(int index) {
  if (index < 0) {
    if (!book.path.empty()) activityManager.goToReader(book.path);
    return;
  }
  if (index >= itemCount()) return;
  switch (kHomeDestinations[index]) {
    case HomeDestination::Library:
      activityManager.goToLibrary();
      break;
    case HomeDestination::Anki:
      activityManager.goToAnki();
      break;
    case HomeDestination::Opds:
      activityManager.goToBrowser();
      break;
    case HomeDestination::Transfer:
      activityManager.goToFileTransfer();
      break;
    case HomeDestination::Tools:
      if (TouchUi::enabled(mappedInput))
        activityManager.goToFileBrowser();
      else
        activityManager.goToTools();
      break;
    case HomeDestination::Settings:
      activityManager.goToSettings();
      break;
  }
}
void HomeActivity::back() {
  if (backPressed && !book.path.empty()) activate(-1);
  backPressed = false;
}
void HomeActivity::loop() {
  MangaCoverInput coverInput(coverWork, mappedInput);
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressed = true;
  if (quickActionsLongPowerHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Power)) {
      quickActionsLongPowerHandled = false;
    }
    return;
  }

  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS &&
      mappedInput.isPressed(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    quickActionsLongPowerHandled = true;
    handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS);
    return;
  }

  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (TouchUi::enabled(mappedInput)) {
    loopTouchHome();
    return;
  }
  bool resume = false;
  int tapX = 0, tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    RenderLock lock(*this);
    resume = !book.path.empty() && mappedInput.wasTapInRect(preview.x, preview.y, preview.width, preview.height);
  }
  if (resume) {
    activate(-1);
    return;
  }
  NavigationListActivity::loop();
}
int HomeActivity::drawAboveList() {
  if (book.path.empty()) return 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const int font = uiScaleSpec().bodyFontId;
  const int line = renderer.getLineHeight(font);
  int topInset, rightInset, bottomInset, leftInset;
  renderer.getOrientedViewableTRBL(&topInset, &rightInset, &bottomInset, &leftInset);
  const int available = renderer.getScreenHeight() - header.y - header.height - metrics.buttonHintsHeight - bottomInset;
  // Keep the preview compact enough for menu rows; the shared list scrolls on short layouts.
  const int height = std::clamp(available - 6 * (line + 16), line * 3, std::max(line * 3, available / 3));
  preview = Rect(leftInset + metrics.contentSidePadding, header.y + header.height,
                 renderer.getScreenWidth() - leftInset - rightInset - metrics.contentSidePadding * 2, height);
  const int h = std::max(1, height - 16);
  const Rect nextCover(preview.x + 8, preview.y + 8, h * 3 / 5, h);
  if (nextCover.width != cover.width || nextCover.height != cover.height) prepared = false;
  cover = nextCover;
  drawBookPreview(renderer, book, cover);
  std::optional<FontCacheManager::PrewarmScope> prewarm;
  if (auto* cache = renderer.getFontCacheManager()) {
    prewarm.emplace(cache->createPrewarmScope());
    renderer.drawText(font, 0, 0, book.title.c_str());
    renderer.drawText(font, 0, 0, book.author.c_str());
    if (!prewarm->endScanAndPrewarm()) {
      LOG_ERR("HOME", "Preview font prewarm failed; using on-demand glyph loading");
    }
  }
  const int x = cover.x + cover.width + 16;
  const Rect textRect(x, preview.y, std::max(1, preview.x + preview.width - x - 8), height);
  const int titleLines = std::clamp((height - (book.author.empty() ? 16 : line + 24)) / line, 1, 2);
  UITheme::drawCenteredWrappedText(renderer, textRect, font, preview.y + 12, book.title.c_str(), titleLines);
  const auto author = renderer.truncatedText(font, book.author.c_str(), textRect.width);
  renderer.drawText(font, x, preview.y + height - line - 8, author.c_str());
  if (selected < 0) renderer.drawRoundedRect(preview.x, preview.y, preview.width, preview.height, 2, 5, true);
  return height + 8;
}
void HomeActivity::present() {
  renderer.displayBuffer(refreshMode);
  refreshMode = HalDisplay::FAST_REFRESH;
}
void HomeActivity::render(RenderLock&& lock) {
  if (quickActionsPopup.processRender(renderer, mappedInput)) return;
  if (TouchUi::enabled(mappedInput))
    renderTouchHome();
  else
    NavigationListActivity::render(std::move(lock));
  if (!book.path.empty() && !prepared) {
    auto batch = coverWork.batch();
    if (batch.cancelled()) return;
    coverWork.active = true;
    prepareBookPreview(book, cover.width, cover.height, renderer, batch.cancellation());
    coverWork.active = false;
    if (!batch.cancelled()) {
      prepared = true;
      requestUpdate();
    }
  }
}
// Menu order follows the approved touch design; indices retain the physical navigation destinations.
namespace {
constexpr int touchMenuDestinations[] = {3, 2, 4, 5};
}

void HomeActivity::loopTouchHome() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (touchMenuOpen) {
      RenderLock lock(*this);
      touchMenuOpen = false;
      selected = firstSelection();
      backPressed = false;
      requestUpdate();
    } else {
      back();
    }
    return;
  }
  int x, y;
  int choice = -2;
  bool light = false;
  if (mappedInput.wasScreenTapped(x, y)) {
    {
      RenderLock lock(*this);
      touchButtonFocus = false;
      const auto hit = [this](const Rect& rect) {
        return mappedInput.wasTapInRect(rect.x, rect.y, rect.width, rect.height);
      };
      if (hit(menuButton)) {
        touchMenuOpen = !touchMenuOpen;
        selected = touchMenuOpen ? touchMenuDestinations[0] : firstSelection();
        requestUpdate();
      } else if (touchMenuOpen) {
        for (int i = 0; i < 4; ++i)
          if (hit(menuRows[i])) choice = touchMenuDestinations[i];
        if (choice == -2) {
          touchMenuOpen = false;
          selected = firstSelection();
          requestUpdate();
        }
      } else if (hit(lightButton)) {
        light = true;
      } else if (hit(continueButton) || (!book.path.empty() && hit(preview))) {
        choice = book.path.empty() ? 0 : -1;
      } else if (hit(libraryButton)) {
        choice = 0;
      } else if (hit(ankiButton)) {
        choice = 1;
      }
    }
    if (light) activityManager.showFrontlightPanel();
    if (choice != -2) activate(choice);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  const auto move = [this](int delta) {
    RenderLock lock(*this);
    touchButtonFocus = true;
    const int first = firstSelection();
    const int count = itemCount() - first;
    selected = first + (selected - first + count + delta) % count;
    touchMenuOpen = selected >= 2;
    requestUpdate();
  };
  navigator.onNextRelease([&] { move(1); });
  navigator.onPreviousRelease([&] { move(-1); });
}

void HomeActivity::renderTouchHome() {
  renderer.clearScreen();
  namespace fui = freeink::ui;
  const auto icon = [&](const freeink::Icon& asset, int x, int y, bool inverted = false) {
    uiTarget.bitmap({x, y, asset.w, asset.h}, fui::bitmapFromIcon(asset), fui::BitmapMode::Center,
                    fui::Paint::solid(inverted ? fui::Color::White : fui::Color::Black));
  };
  int topInset, rightInset, bottomInset, leftInset;
  renderer.getOrientedViewableTRBL(&topInset, &rightInset, &bottomInset, &leftInset);
  const int font = TouchUi::bodyFont();
  const int titleFont = TouchUi::titleFont();
  const int smallFont = TouchUi::smallFont();
  const int line = renderer.getLineHeight(font);
  const int titleLine = renderer.getLineHeight(titleFont);
  const int smallLine = renderer.getLineHeight(smallFont);
  const int screenWidth = renderer.getScreenWidth();
  const int margin = std::max(12, screenWidth * 45 / 1000);
  const int left = leftInset + margin;
  const int width = screenWidth - left - rightInset - margin;
  const int statusBottom = TouchUi::statusHeight(renderer);
  const int top = statusBottom + screenWidth * 5 / 100;
  const int bottom = renderer.getScreenHeight() - bottomInset - margin;
  const int available = bottom - statusBottom;
  const int headerHeight = std::max(titleLine + 12, available * 13 / 100);
  const int control = std::max(44, screenWidth * 14 / 100);
  menuButton = Rect(left + width - control, top, control, headerHeight);
  lightButton = Rect(menuButton.x - control, top, control, headerHeight);
  renderer.drawText(titleFont, left, top + (headerHeight - titleLine) / 2, tr(STR_HOME));
  icon(icon_menu_32, menuButton.x + (control - 32) / 2, top + (headerHeight - 32) / 2);
  icon(icon_brightness_menu_28, lightButton.x + (control - 28) / 2, top + (headerHeight - 28) / 2);
  renderer.drawLine(left, top + headerHeight, left + width, top + headerHeight);

  const int destinationHeight = std::max(72, available * 21 / 100);
  const int summaryHeight = std::max(line + smallLine + 24, available * 16 / 100);
  const int destinationTop = bottom - destinationHeight;
  const int summaryTop = destinationTop - summaryHeight;
  const int buttonHeight = std::max(44, screenWidth * 14 / 100);
  continueButton = Rect(left, summaryTop - buttonHeight - screenWidth * 6 / 100, width, buttonHeight);
  const int bookTop = top + headerHeight + screenWidth * 55 / 1000;
  preview = Rect(left, bookTop, width, std::max(1, continueButton.y - bookTop - 20));
  const auto centered = [&](const Rect& rect, const char* text, int textFont, bool black = true) {
    const auto label = renderer.truncatedText(textFont, text, std::max(1, rect.width - 16));
    renderer.drawText(textFont, rect.x + (rect.width - renderer.getTextWidth(textFont, label.c_str())) / 2,
                      rect.y + (rect.height - renderer.getLineHeight(textFont)) / 2, label.c_str(), black);
  };
  if (!book.path.empty()) {
    const int coverHeight = std::min(preview.height, screenWidth * 43 / 100);
    const Rect nextCover(left, bookTop + (preview.height - coverHeight) / 2, screenWidth * 29 / 100, coverHeight);
    if (nextCover.width != cover.width || nextCover.height != cover.height) prepared = false;
    cover = nextCover;
    drawBookPreview(renderer, book, cover);
    // A typeset cover uses the actual metadata when no thumbnail is available.
    const auto thumbnail = UITheme::getCoverThumbPath(book.coverBmpPath, cover.width, cover.height);
    if (thumbnail.empty() || !Storage.exists(thumbnail.c_str())) {
      renderer.fillRect(cover.x + 1, cover.y + 1, cover.width - 2, cover.height - 2, false);
      centered(Rect(cover.x + 4, cover.y + 8, cover.width - 8, smallLine), book.author.c_str(), smallFont);
      UITheme::drawCenteredWrappedTextAtCenter(renderer, Rect(cover.x + 8, cover.y, cover.width - 16, cover.height),
                                               font, cover.y + cover.height / 2, book.title.c_str(), 3);
      for (int offset : {0, 3})
        renderer.drawLine(cover.x + cover.width / 5, cover.y + cover.height - 20 + offset,
                          cover.x + cover.width * 4 / 5, cover.y + cover.height - 20 + offset);
    }
    const int textX = cover.x + cover.width + screenWidth * 5 / 100;
    const int textWidth = left + width - textX;
    // Wrapping is bounded to three lines; the existing renderer owns these small temporary strings.
    const auto title = renderer.wrappedText(titleFont, book.title.c_str(), textWidth, 3);
    const bool hasProgress = RecentBookProgress::hasPercent(readingPercent);
    const int textHeight = title.size() * titleLine + line + 14 + (hasProgress ? smallLine + 26 : 0);
    int textY = preview.y + std::max(0, (preview.height - textHeight) / 2);
    for (const auto& text : title) {
      renderer.drawText(titleFont, textX, textY, text.c_str());
      textY += titleLine;
    }
    const auto author = renderer.truncatedText(font, book.author.c_str(), textWidth);
    renderer.drawText(font, textX, textY + 10, author.c_str());
    if (hasProgress) {
      const auto percent = RecentBookProgress::formatPercent(readingPercent);
      const int progressY = textY + line + 24;
      renderer.drawText(smallFont, textX, progressY, percent.c_str());
      renderer.fillRectDither(textX, progressY + smallLine + 8, textWidth, 4, LightGray);
      renderer.fillRect(textX, progressY + smallLine + 8, static_cast<int>(textWidth * readingPercent / 100), 4);
    }
  } else {
    const int emptyTitleY = preview.y + std::max(0, (preview.height - titleLine * 2 - line * 2 - 20) / 2);
    const int titleBottom =
        UITheme::drawCenteredWrappedText(renderer, preview, titleFont, emptyTitleY, tr(STR_TOUCH_HOME_EMPTY_TITLE), 2);
    UITheme::drawCenteredWrappedText(renderer, preview, font, emptyTitleY + titleBottom + 20,
                                     tr(STR_TOUCH_HOME_EMPTY_HINT), 2);
  }
  renderer.fillRect(continueButton.x, continueButton.y, continueButton.width, continueButton.height);
  if (book.path.empty()) {
    centered(continueButton, tr(STR_LIBRARY), font, false);
  } else {
    const auto label = renderer.truncatedText(font, tr(STR_CONTINUE_READING), width - 64);
    const int contentWidth = renderer.getTextWidth(font, label.c_str()) + 40;
    const int contentLeft = left + (width - contentWidth) / 2;
    icon(icon_book_open_24, contentLeft, continueButton.y + (buttonHeight - 24) / 2, true);
    renderer.drawText(font, contentLeft + 40, continueButton.y + (buttonHeight - line) / 2, label.c_str(), false);
  }
  if (touchButtonFocus && selected < 0) renderer.drawRect(left - 3, continueButton.y - 3, width + 6, buttonHeight + 6);

  renderer.drawLine(left, summaryTop, left + width, summaryTop);
  const int summaryY = summaryTop + (summaryHeight - smallLine - line - 8) / 2;
  renderer.drawText(smallFont, left, summaryY, tr(STR_ANKI));
  const auto summary = renderer.truncatedText(font, tr(STR_ANKI_CHOOSE_DECK), width);
  renderer.drawText(font, left, summaryY + smallLine + 8, summary.c_str());
  const int gap = screenWidth * 3 / 100;
  libraryButton = Rect(left, destinationTop, (width - gap) / 2, destinationHeight);
  ankiButton = Rect(libraryButton.x + libraryButton.width + gap, destinationTop, width - libraryButton.width - gap,
                    destinationHeight);
  renderer.drawRect(libraryButton.x, libraryButton.y, libraryButton.width, libraryButton.height,
                    touchButtonFocus && selected == 0 ? 3 : 1, true);
  renderer.drawRect(ankiButton.x, ankiButton.y, ankiButton.width, ankiButton.height,
                    touchButtonFocus && selected == 1 ? 3 : 1, true);
  const int iconY = destinationTop + (destinationHeight - 32 - 12 - line) / 2;
  const int libraryX = libraryButton.x + libraryButton.width / 2;
  icon(icon_lyra_library_32, libraryX - 16, iconY);
  const int ankiX = ankiButton.x + ankiButton.width / 2;
  icon(icon_anki_32, ankiX - 16, iconY);
  centered(Rect(libraryButton.x, iconY + 44, libraryButton.width, line), tr(STR_LIBRARY), font);
  centered(Rect(ankiButton.x, iconY + 44, ankiButton.width, line), tr(STR_ANKI), font);

  if (touchMenuOpen) {
    const int panelLeft = screenWidth / 5;
    const int panelRight = left + width;
    const int panelContentLeft = panelLeft + margin;
    renderer.fillRectDither(0, top, panelLeft, renderer.getScreenHeight() - top, LightGray);
    renderer.fillRect(panelLeft, top, screenWidth - panelLeft, renderer.getScreenHeight() - top, false);
    renderer.drawLine(panelLeft, top, panelLeft, renderer.getScreenHeight());
    renderer.drawText(font, panelContentLeft, top + (headerHeight - line) / 2, tr(STR_TOUCH_MENU));
    menuButton = Rect(panelRight - control, top, control, headerHeight);
    centered(menuButton, tr(STR_DONE), font);
    renderer.drawLine(panelContentLeft, top + headerHeight, panelRight, top + headerHeight);
    static constexpr StrId labels[] = {StrId::STR_TRANSFER_FILES, StrId::STR_ONLINE_CATALOGS, StrId::STR_ALL_FILES,
                                       StrId::STR_SETTINGS_TITLE};
    const freeink::Icon* const icons[] = {&icon_transfer_24, &icon_radio_tower_24, &icon_folder_24,
                                          &icon_lyra_settings_24};
    const int rowHeight = std::max(44, screenWidth * 22 / 100);
    for (int i = 0; i < 4; ++i) {
      menuRows[i] =
          Rect(panelContentLeft, top + headerHeight + i * rowHeight + 1, panelRight - panelContentLeft, rowHeight);
      const Rect& row = menuRows[i];
      icon(*icons[i], row.x, row.y + (row.height - 24) / 2);
      const auto label = renderer.truncatedText(font, I18N.get(labels[i]), row.width - 62);
      renderer.drawText(font, row.x + 38, row.y + (row.height - line) / 2, label.c_str());
      const int arrowX = row.x + row.width - 10;
      const int arrowY = row.y + row.height / 2;
      renderer.drawLine(arrowX - 4, arrowY - 6, arrowX + 2, arrowY, 2, true);
      renderer.drawLine(arrowX + 2, arrowY, arrowX - 4, arrowY + 6, 2, true);
      renderer.drawLine(row.x, row.y + row.height - 1, row.x + row.width, row.y + row.height - 1);
      if (touchButtonFocus && selected == touchMenuDestinations[i])
        renderer.drawRect(row.x, row.y, row.width, row.height, 2, true);
    }
  }
  TouchUi::drawStatus(renderer);
  present();
}

bool HomeActivity::handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER) {
    activityManager.goToFileBrowser();
    return true;
  }
  if (action != CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) return false;
  if (quickActionsLongPowerHandled && mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    quickActionsLongPowerHandled = false;
    return true;
  }
  QuickActions::showConfiguredPopup(
      quickActionsPopup, [this] { requestUpdate(); },
      [this](auto selectedAction) {
        if (selectedAction == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH) {
          refreshMode = HalDisplay::FULL_REFRESH;
          requestUpdate();
        } else
          dispatchShortcutAction(selectedAction);
      },
      [](auto selectedAction) {
        return isPowerButtonActionAvailableOutsideReader(selectedAction) ||
               selectedAction == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER;
      });
  return true;
}

std::unique_ptr<Activity> HomeActivity::createFrontlightReadingStatsActivity() {
  const auto& recentBooks = RECENT_BOOKS.getBooks();
  const std::string path = APP_STATE.openEpubPath;
  const bool validEpub = FsHelpers::hasEpubExtension(path) && Storage.exists(path.c_str());
  std::string title = tr(STR_READING_STATS);
  float progress = -1.0f;
  if (validEpub) {
    const auto recent = std::find_if(recentBooks.begin(), recentBooks.end(),
                                     [&path](const RecentBook& book) { return book.path == path; });
    if (recent != recentBooks.end()) {
      title = recent->title;
      progress = RecentBookProgress::loadCachedEpubPercent(*recent);
    } else {
      const size_t slash = path.find_last_of('/');
      title = slash == std::string::npos ? path : path.substr(slash + 1);
    }
  }
  const std::string cachePath = validEpub ? Epub::cachePathForFilePath(path, "/.crosspoint") : std::string{};
  const BookReadingStats bookStats = validEpub ? BookReadingStats::load(cachePath) : BookReadingStats{};
  const GlobalReadingStats deviceStats = GlobalReadingStats::load();
  if (GlobalReadingStats::hasSyncedStats()) {
    return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, bookStats, progress, false, 0,
                                                deviceStats, GlobalReadingStats::loadAggregated(deviceStats));
  }
  return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, bookStats, progress, false, 0,
                                              deviceStats);
}

void HomeActivity::onFrontlightPanelClosed() { requestUpdate(); }

bool HomeActivity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (result.bookPath.empty() || result.action == FrontlightPanelAction::None) return false;
  if (result.action != FrontlightPanelAction::SyncProgress &&
      result.action != FrontlightPanelAction::NearbyPositionSync &&
      result.action != FrontlightPanelAction::SendNearbyBook) {
    return false;
  }

  PendingOverlayResume resume;
  resume.origin = PendingOverlayOrigin::Home;
  resume.overlay = PendingOverlayType::FrontlightDrawer;
  resume.selectedIndex = result.state.selectedAction;
  resume.bookPath = result.bookPath;
  resume.returnHomeAfterReaderFlow = result.action == FrontlightPanelAction::NearbyPositionSync;
  if (result.action == FrontlightPanelAction::SyncProgress) {
    if (KOREADER_STORE.hasCredentials()) APP_STATE.setPendingOverlayResume(resume);
    return startGlobalSyncProgress();
  }
  if (result.action == FrontlightPanelAction::NearbyPositionSync) {
    activityManager.goToReaderAndRunMenuAction(result.bookPath,
                                               static_cast<uint8_t>(EpubReaderMenuAction::NEARBY_POSITION_SYNC));
    APP_STATE.setPendingOverlayResume(std::move(resume));
    return true;
  }
  if (!activityManager.goToNearbyBookSend(result.bookPath, false)) return false;
  APP_STATE.setPendingOverlayResume(std::move(resume));
  return true;
}
