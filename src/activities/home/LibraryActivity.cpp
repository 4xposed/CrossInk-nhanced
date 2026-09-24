#include "LibraryActivity.h"

#include <I18n.h>
#include <MangaBook.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "BookActions.h"
#include "BookPreview.h"
#include "MangaCoverInput.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/BookFolderMutation.h"
namespace {
constexpr StrId TAB_LABELS[] = {StrId::STR_LIBRARY_ALL, StrId::STR_LIBRARY_MANGA, StrId::STR_LIBRARY_BOOKS,
                                StrId::STR_LIBRARY_ARTICLES};
bool contains(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height; }
}  // namespace
void LibraryActivity::onEnter() {
  Activity::onEnter();
  // ~3 KiB catalog scratch and one 512-byte entry; lifetime is one Library visit.
  catalog = makeUniqueNoThrow<library::LibraryCatalog>();
  entry = makeUniqueNoThrow<library::CatalogEntry>();
  tabs.reserve(4);
  rebuild();
}
void LibraryActivity::rebuild() {
  RenderLock lock(*this);
  loadedPage = UINT32_MAX;
  pageSize = 0;
  prepared.fill(false);
  books.fill({});
  if (!catalog) catalog = makeUniqueNoThrow<library::LibraryCatalog>();
  if (!entry) entry = makeUniqueNoThrow<library::CatalogEntry>();
  if (!catalog || !entry)
    LOG_ERR("Library", "Cannot allocate catalog");
  else
    catalog->begin(SETTINGS.libraryMangaFolder, SETTINGS.libraryBooksFolder, SETTINGS.libraryArticlesFolder,
                   SETTINGS.showHiddenFiles);
  coverWork.authorizeIntent();
  requestUpdate();
}
void LibraryActivity::onExit() {
  if (catalog) catalog->close();
  catalog.reset();
  entry.reset();
  Activity::onExit();
}
void LibraryActivity::updateTabs() {
  const auto oldCategory = tabs.empty() ? library::Category::All : categories[selectedTab];
  tabs.clear();
  selectedTab = 0;
  for (int i = 0; i < 4; ++i) {
    const auto category = static_cast<library::Category>(i);
    if (i && !catalog->count(category)) continue;
    if (category == oldCategory) selectedTab = tabs.size();
    categories[tabs.size()] = category;
    tabs.push_back({I18N.get(TAB_LABELS[i]), false});
  }
  tabs[selectedTab].selected = true;
  navigation.count = catalog->count(categories[selectedTab]);
  navigation.clamp();
}
void LibraryActivity::changeTab(int delta) {
  if (tabs.empty()) return;
  tabs[selectedTab].selected = false;
  selectedTab = (selectedTab + static_cast<int>(tabs.size()) + delta) % tabs.size();
  tabs[selectedTab].selected = true;
  navigation.count = catalog->count(categories[selectedTab]);
  navigation.selected = 0;
  navigation.clamp();
  loadedPage = UINT32_MAX;
  requestUpdate();
}
void LibraryActivity::loop() {
  MangaCoverInput coverInput(coverWork, mappedInput);
  // Do not park the main/input task behind an idle render doing cover I/O.
  // The next input edge will cancel and drain that work through the shared guard.
  if (!coverInput.hasInput() && RenderLock::peek()) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, TouchHeaderBackButton::compactHeaderRect(renderer)) ||
      (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= 1000)) {
    onGoHome(HomeMenuItem::LIBRARY);
    return;
  }
  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back);
  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  int tx = 0, ty = 0;
  const bool tap = mappedInput.wasScreenTapped(tx, ty);
  int lx = 0, ly = 0;
  const bool heldTouch = mappedInput.isScreenTouchLongPress(lx, ly, 1000);
  const bool heldConfirm =
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 1000;
  bool open = false, actions = false, retry = false, home = false;
  {
    RenderLock lock(*this);
    if (!catalog || !entry || catalog->state() == library::ScanState::Failed) {
      retry = confirm;
      home = back;
    } else if (catalog->state() == library::ScanState::Scanning) {
      if (back)
        home = true;
      else if (catalog->step(12) != library::ScanState::Scanning) {
        updateTabs();
        requestUpdate();
      }
    } else {
      if (back) {
        if (navigation.tabsFocused)
          home = true;
        else {
          navigation.tabsFocused = true;
          requestUpdate();
        }
      }
      if (tap) {
        int index;
        if (GUI.tabIndexFromPoint(renderer, tabRect, tabs, tx, ty, index)) {
          changeTab(index - selectedTab);
          navigation.tabsFocused = true;
        } else
          for (int i = 0; i < pageSize; ++i)
            if (contains(cells[i], tx, ty)) {
              navigation.selected = loadedPage + i;
              navigation.tabsFocused = false;
              open = true;
              break;
            }
      }
      if (heldTouch && !longPressHandled) {
        for (int i = 0; i < pageSize; ++i)
          if (contains(cells[i], lx, ly)) {
            navigation.selected = loadedPage + i;
            navigation.tabsFocused = false;
            mappedInput.suppressNextTouchTap();
            actions = true;
            break;
          }
      }
      if (heldConfirm && !navigation.tabsFocused && !longPressHandled) actions = true;
      if (actions) longPressHandled = true;
      if (confirm) {
        if (longPressHandled)
          longPressHandled = false;
        else if (navigation.tabsFocused) {
          navigation.down();
          requestUpdate();
        } else
          open = true;
      }
      const auto move = [this](int delta) {
        if (navigation.tabsFocused)
          changeTab(delta);
        else
          navigation.move(delta);
        requestUpdate();
      };
      navigator.onRelease({MappedInputManager::Button::Left}, [&] { move(-1); });
      navigator.onRelease({MappedInputManager::Button::Right}, [&] { move(1); });
      navigator.onRelease({MappedInputManager::Button::Up}, [this] {
        navigation.up();
        requestUpdate();
      });
      navigator.onRelease({MappedInputManager::Button::Down}, [this] {
        navigation.down();
        requestUpdate();
      });
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
        navigation.tabsFocused = false;
        navigation.move(navigation.capacity);
        requestUpdate();
      } else if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
        navigation.tabsFocused = false;
        navigation.move(-navigation.capacity);
        requestUpdate();
      }
    }
  }
  if (home)
    onGoHome(HomeMenuItem::LIBRARY);
  else if (retry)
    rebuild();
  else if (actions)
    showActions();
  else if (open)
    openSelected();
}
void LibraryActivity::openSelected() {
  std::string path;
  {
    RenderLock lock(*this);
    if (navigation.count && loadedPage != UINT32_MAX && navigation.selected >= loadedPage &&
        navigation.selected - loadedPage < static_cast<uint32_t>(pageSize))
      path = books[navigation.selected - loadedPage].path;
  }
  if (path.empty()) return;
  if (Storage.exists(path.c_str()))
    activityManager.goToReader(path);
  else {
    LOG_ERR("Library", "Book disappeared: %s", path.c_str());
    rebuild();
  }
}
void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::drawCompact(renderer, tr(STR_LIBRARY));
  else
    CompactHeader::drawTitle(renderer, tr(STR_LIBRARY));
  const int font = uiScaleSpec().bodyFontId;
  const int line = renderer.getLineHeight(font);
  auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  int insetTop, insetRight, insetBottom, insetLeft;
  renderer.getOrientedViewableTRBL(&insetTop, &insetRight, &insetBottom, &insetLeft);
  const int right = std::min(safe.x + safe.width, renderer.getScreenWidth() - insetRight);
  const int bottom = std::min(safe.y + safe.height, renderer.getScreenHeight() - insetBottom);
  safe.x = std::max(safe.x, insetLeft);
  safe.y = std::max(safe.y, insetTop);
  safe.width = right - safe.x;
  safe.height = bottom - safe.y;
  const int top = std::max(safe.y, CompactHeader::contentTop(metrics));
  tabRect = Rect(safe.x, top, safe.width, std::max(line + 12, metrics.tabBarHeight));
  body = Rect(safe.x + metrics.contentSidePadding, tabRect.y + tabRect.height + 8,
              std::max(1, safe.width - 2 * metrics.contentSidePadding),
              std::max(1, safe.y + safe.height - (tabRect.y + tabRect.height + 8) - line - 8));
  const auto layout = library::gridLayout(body.width, body.height, line, 8);
  if (layout.capacity() != navigation.capacity || layout.coverWidth != grid.coverWidth ||
      layout.coverHeight != grid.coverHeight)
    loadedPage = UINT32_MAX;
  grid = layout;
  navigation.capacity = grid.capacity();
  navigation.columns = grid.columns;
  navigation.clamp();
  const bool ready = catalog && entry && catalog->state() == library::ScanState::Ready;
  if (!ready) {
    const bool scanning = catalog && catalog->state() == library::ScanState::Scanning;
    renderer.drawText(font, body.x, body.y, scanning ? tr(STR_LIBRARY_SCANNING) : tr(STR_LIBRARY_ERROR));
  } else {
    GUI.drawTabBar(renderer, tabRect, tabs, navigation.tabsFocused);
    if (!navigation.count) renderer.drawText(font, body.x, body.y, tr(STR_LIBRARY_EMPTY));
    if (loadedPage != navigation.pageStart()) {
      loadedPage = navigation.pageStart();
      prepared.fill(false);
      pageSize = 0;
      for (int i = 0; i < navigation.capacity && loadedPage + i < navigation.count; ++i) {
        if (!catalog->entryAt(categories[selectedTab], loadedPage + i, *entry)) break;
        books[i] = {};
        books[i].path = entry->path;
        books[i].title = books[i].path.substr(books[i].path.find_last_of('/') + 1);
        ++pageSize;
      }
      if (catalog->state() != library::ScanState::Ready) {
        requestUpdate();
        return;
      }
    }
    for (int i = 0; i < pageSize; ++i) {
      const int x = body.x + (i % grid.columns) * grid.cellWidth;
      const int y = body.y + (i / grid.columns) * grid.cellHeight;
      cells[i] = Rect(x, y, grid.cellWidth, grid.cellHeight);
      drawBookPreview(renderer, books[i],
                      Rect(x + (grid.cellWidth - grid.coverWidth) / 2, y + 4, grid.coverWidth, grid.coverHeight));
      UITheme::drawCenteredWrappedText(renderer, Rect(x + 4, y, grid.cellWidth - 8, grid.cellHeight), font,
                                       y + grid.coverHeight + 8, books[i].title.c_str(), 2);
      if (!navigation.tabsFocused && navigation.selected == loadedPage + i)
        renderer.drawRoundedRect(x + 1, y + 1, grid.cellWidth - 2, grid.cellHeight - 2, 2, 4, true);
    }
    if (navigation.count) {
      char page[40];
      snprintf(page, sizeof(page), "%lu / %lu", static_cast<unsigned long>(loadedPage / navigation.capacity + 1),
               static_cast<unsigned long>((navigation.count + navigation.capacity - 1) / navigation.capacity));
      renderer.drawCenteredText(font, body.y + body.height, page);
    }
  }
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(navigation.tabsFocused ? tr(STR_HOME) : tr(STR_LIBRARY_TABS)),
                            tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (actionFailed) GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  renderer.displayBuffer();
  if (ready)
    for (int i = 0; i < pageSize; ++i)
      if (!prepared[i]) {
        auto batch = coverWork.batch();
        if (batch.cancelled()) break;
        coverWork.active = true;
        prepareBookPreview(books[i], grid.coverWidth, grid.coverHeight, renderer, batch.cancellation());
        coverWork.active = false;
        if (!batch.cancelled()) prepared[i] = true;
        requestUpdate();
        break;  // One book per render, allowing input/power handling between books.
      }
}
void LibraryActivity::showActions() {
  RecentBook book;
  {
    RenderLock lock(*this);
    if (!navigation.count || loadedPage == UINT32_MAX || navigation.selected < loadedPage ||
        navigation.selected - loadedPage >= static_cast<uint32_t>(pageSize))
      return;
    book = books[navigation.selected - loadedPage];
  }
  auto items = BookActions::buildBookActionItems(book.path, false);
  if (BookActions::canSendNearby(book.path))
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  auto popup = makeUniqueNoThrow<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items), true);
  if (!popup) {
    LOG_ERR("Library", "Cannot allocate actions");
    return;
  }
  startActivityForResult(std::move(popup), [this, book](const ActivityResult& result) {
    longPressHandled = false;
    if (result.isCancelled) return;
    if (const auto* action = std::get_if<FileBrowserActionResult>(&result.data)) runAction(action->action, book);
  });
}
void LibraryActivity::runAction(int value, const RecentBook& book) {
  const auto action = static_cast<FileBrowserAction>(value);
  if (action == FileBrowserAction::SendNearby) {
    activityManager.goToNearbyBookSend(book.path, false);
    return;
  }
  if (action == FileBrowserAction::ToggleCompleted) {
    BookActions::startCompletionEdit(*this, renderer, mappedInput, book.path, book.title,
                                     [this](const ActivityResult&) { rebuild(); });
    return;
  }
  if (action == FileBrowserAction::EpubRenderMode) {
    auto popup = makeUniqueNoThrow<OptionSelectionActivity>(
        renderer, mappedInput, "EpubRenderMode", StrId::STR_EPUB_RENDER_MODE, BookActions::epubRenderModeOptions(),
        BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(book.path)));
    if (!popup) {
      LOG_ERR("Library", "Cannot allocate render-mode picker");
      return;
    }
    startActivityForResult(std::move(popup), [this, book](const ActivityResult& result) {
      if (!result.isCancelled)
        if (const auto* option = std::get_if<OptionSelectionResult>(&result.data)) {
          actionFailed = !EpubReaderActivity::saveBookRenderMode(
              book.path, BookActions::epubRenderModeForDisplayIndex(option->index));
        }
      rebuild();
    });
    return;
  }
  const StrId label = action == FileBrowserAction::Delete        ? StrId::STR_DELETE
                      : action == FileBrowserAction::DeleteCache ? StrId::STR_DELETE_CACHE
                      : action == FileBrowserAction::DeleteStats ? StrId::STR_DELETE_BOOK_STATS
                                                                 : StrId::STR_RESET_BOOK_READER_SETTINGS;
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, BookActions::confirmationHeading(label),
                                                         book.title);
  if (!confirm) {
    LOG_ERR("Library", "Cannot allocate confirmation");
    return;
  }
  startActivityForResult(std::move(confirm), [this, book, action](const ActivityResult& result) {
    if (result.isCancelled) return;
    bool ok = false;
    switch (action) {
      case FileBrowserAction::Delete:
        if (manga::MangaBook::isMangaFolder(book.path.c_str()))
          ok = BookFolderMutation::remove(book.path.c_str()) == BookFolderMutation::Result::Complete;
        else if ((ok = Storage.remove(book.path.c_str()))) {
          BookActions::clearFileMetadata(book.path);
          RECENT_BOOKS.removeByPath(book.path);
        }
        break;
      case FileBrowserAction::DeleteCache:
        ok = BookActions::clearBookCache(book.path);
        break;
      case FileBrowserAction::DeleteStats:
        ok = BookActions::deleteBookStats(book.path);
        break;
      case FileBrowserAction::ResetReaderSettings:
        ok = BookActions::resetBookReaderSettings(book.path);
        break;
      default:
        break;
    }
    actionFailed = !ok;
    if (!ok) LOG_ERR("Library", "Book action failed: %s", book.path.c_str());
    rebuild();
  });
}
