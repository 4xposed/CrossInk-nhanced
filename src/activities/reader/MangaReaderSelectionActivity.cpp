#include "MangaReaderSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr uint32_t MAX_MANGA_PAGE = 9999;
constexpr uint16_t MAX_ENCODED_PANEL = 255;
}  // namespace

MangaReaderSelectionActivity::MangaReaderSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           manga::MangaBook& book, BookmarkStore& bookmarkStore,
                                                           const bool showBookmarks, const uint32_t currentPage)
    : Activity("MangaReaderSelection", renderer, mappedInput),
      book(book),
      bookmarkStore(bookmarkStore),
      showBookmarks(showBookmarks),
      currentPage(currentPage),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int MangaReaderSelectionActivity::totalItems() const {
  return showBookmarks ? static_cast<int>(bookmarkStore.getBookmarks().size()) : static_cast<int>(book.tocCount());
}

int MangaReaderSelectionActivity::findInitialIndex() const {
  if (showBookmarks) {
    const auto& bookmarks = bookmarkStore.getBookmarks();
    for (size_t i = 0; i < bookmarks.size(); ++i) {
      if (bookmarks[i].spineIndex == currentPage) return static_cast<int>(i);
    }
    return 0;
  }

  int current = 0;
  manga::format::TocEntryView entry{};
  for (uint32_t i = 0; i < book.tocCount(); ++i) {
    if (!book.readTocEntry(i, entry)) continue;
    if (entry.pageIndex > currentPage) break;
    if (entry.pageIndex <= MAX_MANGA_PAGE && entry.pageIndex < book.pageCount()) current = static_cast<int>(i);
  }
  return current;
}

void MangaReaderSelectionActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  mappedInput.setReaderTouchscreenOverride(true);
  selectorIndex = findInitialIndex();
  topIndex = 0;
  visibleRows = 1;
  initialViewportPending = true;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &MangaReaderSelectionActivity::onRowEvent, this);
  app.setScreen(&MangaReaderSelectionActivity::listScreen, this);
  requestUpdate();
}

void MangaReaderSelectionActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void MangaReaderSelectionActivity::selectItem() {
  if (selectorIndex < 0 || selectorIndex >= totalItems()) return;

  if (showBookmarks) {
    const Bookmark& bookmark = bookmarkStore.getBookmarks()[static_cast<size_t>(selectorIndex)];
    if (bookmark.spineIndex >= book.pageCount() || bookmark.spineIndex > MAX_MANGA_PAGE ||
        bookmark.paragraphIndex == UINT16_MAX || bookmark.paragraphIndex > MAX_ENCODED_PANEL) {
      return;
    }
    setResult(BookmarkResult{bookmark.spineIndex, bookmark.progress, bookmark.paragraphIndex});
    finish();
    return;
  }

  manga::format::TocEntryView entry{};
  if (!book.readTocEntry(static_cast<uint32_t>(selectorIndex), entry) || entry.pageIndex >= book.pageCount() ||
      entry.pageIndex > MAX_MANGA_PAGE) {
    return;
  }
  setResult(BookmarkResult{static_cast<uint16_t>(entry.pageIndex), 0.0f, 0});
  finish();
}

void MangaReaderSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<MangaReaderSelectionActivity*>(user);
  if (event.value < 0 || event.value >= self->totalItems()) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->selectItem();
}

void MangaReaderSelectionActivity::loop() {
  RenderLock lock(*this);
  const int count = totalItems();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, count);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }
  const auto moveSelection = [this, count](const int index) {
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, count, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, count)); });
  buttonNavigator.onNextContinuous([this, count, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, count, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, count, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, count, visibleRows));
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) selectItem();
}

void MangaReaderSelectionActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<MangaReaderSelectionActivity*>(user)->buildListScreen(screen);
}

void MangaReaderSelectionActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = totalItems();
  if (count == 0) {
    screen.centeredText(showBookmarks ? tr(STR_NO_BOOKMARKS) : tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = initialViewportPending ? followListSelection(selectorIndex, 0, visibleRows, count)
                                    : scrollListBy(topIndex, 0, visibleRows, count);
  initialViewportPending = false;
  const size_t drawCount =
      std::min({static_cast<size_t>(visibleRows), ROW_WINDOW_SIZE, static_cast<size_t>(count - topIndex)});
  const auto& bookmarks = bookmarkStore.getBookmarks();
  for (size_t i = 0; i < drawCount; ++i) {
    const size_t sourceIndex = static_cast<size_t>(topIndex) + i;
    itemWindow[i] = fui::ListItem{};
    if (showBookmarks) {
      const Bookmark& bookmark = bookmarks[sourceIndex];
      itemWindow[i].label = bookmark.snippet[0] != '\0'
                                ? bookmark.snippet
                                : (bookmark.chapterTitle[0] != '\0' ? bookmark.chapterTitle : tr(STR_UNNAMED));
      char* value = valueWindow.data() + i * 8;
      std::snprintf(value, 8, "%u", static_cast<unsigned>(bookmark.spineIndex + 1));
      itemWindow[i].value = value;
    } else {
      manga::format::TocEntryView entry{};
      if (book.readTocEntry(static_cast<uint32_t>(sourceIndex), entry)) {
        size_t copied = std::min(entry.title.size(), TOC_LABEL_SIZE - 1);
        copied = static_cast<size_t>(utf8SafeTruncateBuffer(entry.title.data(), static_cast<int>(copied)));
        std::memcpy(tocLabels[i].data(), entry.title.data(), copied);
        tocLabels[i][copied] = '\0';
        itemWindow[i].label = copied == 0 ? tr(STR_UNNAMED) : tocLabels[i].data();
      } else {
        tocLabels[i][0] = '\0';
        itemWindow[i].label = tr(STR_UNNAMED);
      }
    }
    itemWindow[i].actionValue = static_cast<int16_t>(sourceIndex);
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(drawCount);
  props.selectedIndex = selectorIndex - topIndex;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), screen.body(), static_cast<size_t>(count), visibleRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void MangaReaderSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  const char* title = showBookmarks ? tr(STR_BOOKMARKS) : tr(STR_SELECT_CHAPTER);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, true);
  } else {
    GUI.drawHeader(renderer, header, title, nullptr, true);
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            totalItems() == 0 ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
