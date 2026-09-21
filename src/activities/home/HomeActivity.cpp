#include "HomeActivity.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <Memory.h>
#include "RecentBookProgress.h"
#include "KOReaderCredentialStore.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/EpubReaderUtils.h"

#include <algorithm>
#include <optional>

#include "BookPreview.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "HomeNavigation.h"
#include "MangaCoverInput.h"
#include "QuickActions.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
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
  coverWork.authorizeIntent();
  requestUpdate();
}
void HomeActivity::onExit() { Activity::onExit(); }
const char* HomeActivity::itemLabel(int index) const {
  static constexpr StrId labels[] = {StrId::STR_LIBRARY,        StrId::STR_ANKI,  StrId::STR_OPDS_BROWSER,
                                     StrId::STR_TRANSFER_FILES, StrId::STR_TOOLS, StrId::STR_SETTINGS_TITLE};
  return I18N.get(labels[index]);
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

void HomeActivity::onFrontlightPanelClosed() {
  requestUpdate();
}

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
