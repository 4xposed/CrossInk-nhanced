#include "MangaReaderActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <MangaBitmapPixels.h>
#include <MangaCover.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>

#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderWordLookupActivity.h"
#include "LookedUpWordsActivity.h"
#include "MangaQrPayload.h"
#include "MangaReaderSelectionActivity.h"
#include "MangaRegionSelectionActivity.h"
#include "MangaStatus.h"
#include "MangaTranslationActivity.h"
#include "PageTextViewport.h"
#include "QrDisplayActivity.h"
#include "ReaderOptionsActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsSave.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
using manga::MenuAction;
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
constexpr unsigned long LONG_PRESS_MENU_MS = 600UL;

std::string bookmarkMetadataLabel(std::string_view text) {
  // Portable metadata fields can be 64 KiB each. Bookmark headers need only a
  // display label; do not duplicate the adapter's entire optional metadata buffer.
  const int bytes = utf8SafeTruncateBuffer(text.data(), static_cast<int>(std::min<size_t>(text.size(), 127)));
  return bytes > 0 ? std::string(text.data(), static_cast<size_t>(bytes)) : std::string{};
}

std::string recentMetadataLabel(std::string_view text) { return bookmarkMetadataLabel(text); }

#ifdef SIMULATOR
uint32_t mangaFramebufferHash(const GfxRenderer& renderer) {
  constexpr uint32_t FNV_OFFSET = 2166136261U;
  constexpr uint32_t FNV_PRIME = 16777619U;
  uint32_t hash = FNV_OFFSET;
  const uint8_t* bytes = renderer.getFrameBuffer();
  for (size_t i = 0; bytes && i < renderer.getBufferSize(); ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

void logMangaFramebufferHash(const GfxRenderer& renderer, const uint32_t page, const int16_t panel,
                             const GfxRenderer::Orientation orientation, const char* plane) {
  LOG_INF("MANGA", "Grayscale hash page=%lu panel=%d orientation=%d plane=%s hash=%08lx",
          static_cast<unsigned long>(page), panel, static_cast<int>(orientation), plane,
          static_cast<unsigned long>(mangaFramebufferHash(renderer)));
}
#endif
}  // namespace

MangaReaderActivity::MangaReaderActivity(GfxRenderer& renderer, MappedInputManager& input, std::string folder)
    : Activity("MangaReader", renderer, input), folder(std::move(folder)), progressStore(this->folder) {}

void MangaReaderActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  entryOrientation = renderer.getOrientation();
  pathCapacity = folder.size() + 258;
  path = makeUniqueNoThrow<char[]>(pathCapacity);
  if (!path || !book.open(folder.c_str())) {
    LOG_ERR("MANGA", "Cannot open manga reader: %s", folder.c_str());
    requestUpdate();
    return;
  }
  // Existing decoder API requires a string. Reserve its maximum once per session.
  imagePath.reserve(pathCapacity);
  imageConfig.cachePath.reserve(128);
  // <=8192 raw BMP bytes +512 packed gray bytes +512 output bytes. Reuse for
  // every page and plane instead of allocating rows repeatedly on the C3 heap.
  pixelScratch = makeUniqueNoThrow<uint8_t[]>(manga::kBitmapPixelScratchBytes);
  if (!pixelScratch) LOG_ERR("MANGA", "Pixel scratch unavailable; keeping BW rendering");
  progressStore.load(progress);
  position = {progress.page < book.pageCount() ? progress.page : 0, progress.panel};
  loadPageLocked(position.page);
  position = manga::normalizePosition(position, available, progress.panelsOnly);
  BOOKMARKS.loadForBook(folder, bookmarkMetadataLabel(book.title()), bookmarkMetadataLabel(book.author()), "manga");
  mappedInput.setReaderMode(true);
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) mappedInput.suppressNextConfirmRelease();
  ready = true;
  observeProgressLocked();
  statsCachePath = manga::cachePath(folder);
  if (statsCachePath.empty() || !Storage.ensureDirectoryExists("/.crosspoint") ||
      !Storage.ensureDirectoryExists(statsCachePath.c_str())) {
    LOG_ERR("MANGA", "Cannot prepare manga reading stats cache");
    statsCachePath.clear();
  }
  if (!statsCachePath.empty()) stats = BookReadingStats::load(statsCachePath);
  globalStats = GlobalReadingStats::load();
  sessionReadingMs = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
  APP_STATE.openEpubPath = folder;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addOrUpdateBook(folder, recentMetadataLabel(book.title()), recentMetadataLabel(book.author()),
                               manga::thumbnailTemplatePath(folder));
  // One bounded context lives across jobs; its paths and BMP rows exceed the
  // small stack and cannot be static across activity lifetimes.
  prefetch = makeUniqueNoThrow<manga::MangaPrefetch>(folder, pathCapacity);
  if (!prefetch || !prefetch->start()) {
    LOG_ERR("MANGA", "Prefetch unavailable; foreground reading remains enabled");
    prefetch.reset();
  }
  resumeReadingStatsTimer();
  requestUpdate();
}

void MangaReaderActivity::onExit() {
  autoTurn.cancel();
  // Cancel/join before progress, stats, bookmarks, source buffers or renderer.
  prefetch.reset();
  // ActivityManager already holds RenderLock while exiting/destroying activities.
  if (ready && saveDebouncer.hasPending()) saveProgressLocked();
  if (!commitReadingStats()) LOG_ERR("MANGA", "Stats remain unsaved at forced reader exit");
  BOOKMARKS.saveToFile();
  BOOKMARKS.unload();
  pixelCache.close();
  pixelCache.discardTemporary();
  pixelScratch.reset();
  book.close();
  path.reset();
  renderer.setOrientation(entryOrientation);
  mappedInput.setReaderMode(false);
  mappedInput.setReaderTouchscreenOverride(false);
  APP_STATE.readerActivityLoadCount = 0;
  Activity::onExit();
}

bool MangaReaderActivity::loadPageLocked(uint32_t number) {
  available = {};
  page = {};
  position.page = number;
  const bool loaded = book.loadPage(number, page);
  available.overview = book.pageImagePath(number, path.get(), pathCapacity) == manga::PathResult::Found;
  if (loaded) {
    available.panelCount = page.panels.remaining;
    for (uint16_t i = 0; i < available.panelCount; ++i) {
      if (book.panelImagePath(number, i, path.get(), pathCapacity) == manga::PathResult::Found) available.setCrop(i);
    }
  }
  imageDirty = true;
  return loaded;
}

void MangaReaderActivity::observeProgressLocked() {
  progress.page = position.page;
  progress.panel = position.panel;
  // Count physical page changes for the ten-page cadence. Panel/settings changes
  // remain pending metadata and flush on the timer or when leaving the reader.
  const uint32_t metadata = static_cast<uint32_t>(position.panel + 1) |
                            (static_cast<uint32_t>(progress.panelsOnly) << 8) |
                            (static_cast<uint32_t>(progress.rotatePanels) << 9);
  const bool due = saveDebouncer.observe(position.page, metadata);
  constexpr unsigned long SAVE_RETRY_MS = 30000;
  if (due && (!saveFailed || millis() - lastSaveFailureMs >= SAVE_RETRY_MS)) saveProgressLocked();
}

bool MangaReaderActivity::saveProgressLocked() {
  if (!progressStore.save(progress)) {
    saveFailed = true;
    lastSaveFailureMs = millis();
    LOG_ERR("MANGA", "Failed to save reading position; retrying in 30 seconds");
    return false;
  }
  saveFailed = false;
  saveDebouncer.markPersisted(saveDebouncer.lastObservedPosition(), saveDebouncer.lastObservedMetadata());
  return true;
}

void MangaReaderActivity::move(bool forward) {
  RenderLock lock(*this);
  pendingInput.move(forward);
  foregroundReadyLocked();
}

void MangaReaderActivity::moveLocked(bool forward) {
  const auto target = forward ? manga::next(position, book.pageCount(), available, progress.panelsOnly)
                              : manga::previous(position, book.pageCount(), available, progress.panelsOnly);
  applyMoveLocked(target, forward);
}

void MangaReaderActivity::applyMoveLocked(const manga::Move& target, const bool forward) {
  if (!target.changed) {
    if (forward && book.pageCount() > 0 && position.page + 1 >= book.pageCount()) {
      recordCurrentPageReadingTime();
      if (!finalPageCounted && physicalPageReadingMs >= MIN_READING_STATS_PAGE_MS) {
        recordForwardPageTurn(physicalPageReadingMs / 1000UL);
        finalPageCounted = true;
      }
      setBookCompleted(true);
      resumeReadingStatsTimer();
    }
    return;
  }
  if (target.changePage) {
    recordCurrentPageReadingTime();
    const uint32_t forwardReadSeconds = physicalPageReadingMs / 1000UL;
    physicalPageReadingMs = 0;
    finalPageCounted = false;
    loadPageLocked(target.position.page);
    position = manga::resolveEntry(target.position.page, target.entry, available, progress.panelsOnly);
    if (forward && forwardReadSeconds >= MIN_READING_STATS_PAGE_MS / 1000UL) recordForwardPageTurn(forwardReadSeconds);
  } else {
    // Panel views belong to the same physical page. Bank their elapsed time, but
    // keep one page-level pace sample and counter for the eventual page crossing.
    recordCurrentPageReadingTime();
    position = target.position;
  }
  imageDirty = true;
  resumeReadingStatsTimer();
  observeProgressLocked();
  requestUpdate();
}

void MangaReaderActivity::jump(uint32_t number, int16_t panel) {
  RenderLock lock(*this);
  if (!ready || number >= book.pageCount()) return;
  physicalPageReadingMs = 0;
  finalPageCounted = false;
  loadPageLocked(number);
  position = manga::normalizePosition({number, panel}, available, progress.panelsOnly);
  observeProgressLocked();
  requestUpdate();
}

bool MangaReaderActivity::openReaderSettingsMenu() {
  RenderLock lock(*this);
  if (!ready || inputLocked || suspended || childActive) return false;
  autoTurn.cancel();
  if (menu.isActive()) return true;
  pendingLookup = false;
  pendingInput.requestMenu();
  foregroundReadyLocked();
  return true;
}

void MangaReaderActivity::showMenuLocked() {
  pauseReadingStatsTimer();
  char autoLabel[80];
  const StrId rateLabels[] = {StrId::STR_OFF, StrId::STR_MANGA_AUTO_ONE, StrId::STR_MANGA_AUTO_THREE,
                              StrId::STR_MANGA_AUTO_SIX, StrId::STR_MANGA_AUTO_TWELVE};
  snprintf(autoLabel, sizeof(autoLabel), "%s %s", tr(STR_AUTO_TURN_ENABLED),
           I18n::getInstance().get(rateLabels[autoTurn.rateIndex()]));
  const char* options[] = {tr(STR_SELECT_CHAPTER),
                           tr(STR_GO_TO_PERCENT),
                           tr(STR_VIEW_BOOKMARKS),
                           tr(STR_TOGGLE_BOOKMARK),
                           progress.panelsOnly ? tr(STR_MANGA_SHOW_OVERVIEWS) : tr(STR_MANGA_PANELS_ONLY),
                           progress.rotatePanels ? tr(STR_MANGA_ROTATION_OFF) : tr(STR_MANGA_ROTATION_ON),
                           tr(STR_ORIENTATION),
                           tr(STR_HOME),
                           tr(STR_LOOKUP),
                           tr(STR_MANGA_TRANSLATION),
                           tr(STR_LOOKUP_HISTORY),
                           tr(STR_READER_OPTIONS),
                           autoLabel,
                           tr(STR_SCREENSHOT_BUTTON),
                           tr(STR_DELETE_CACHE),
                           tr(STR_DISPLAY_QR)};
  static_assert(sizeof(options) / sizeof(options[0]) == manga::kMenuActionCount);
  menu.show(tr(STR_READER_MENU), options, manga::kMenuActionCount, 0,
            [this](int action) { pendingMenuAction = manga::menuActionAt(action); });
  requestUpdate();
}

void MangaReaderActivity::showSelection(bool bookmarks) {
  auto activity =
      makeUniqueNoThrow<MangaReaderSelectionActivity>(renderer, mappedInput, book, BOOKMARKS, bookmarks, position.page);
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate selection screen");
    resumeReadingStatsTimer();
    return;
  }
  childActive = true;
  autoTurn.cancel();
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      if (const auto* target = std::get_if<BookmarkResult>(&result.data)) {
        jump(target->spineIndex, static_cast<int16_t>(target->paragraphIndex) - 1);
      }
    }
    childReturned();
  });
}

void MangaReaderActivity::toggleBookmark() {
  RenderLock lock(*this);
  const auto& bookmarks = BOOKMARKS.getBookmarks();
  const uint16_t anchor = static_cast<uint16_t>(position.panel + 1);
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    if (bookmarks[i].spineIndex == position.page && bookmarks[i].paragraphIndex == anchor) {
      BOOKMARKS.removeBookmarkAt(i);
      return;
    }
  }
  char label[64];
  snprintf(label, sizeof(label), tr(STR_BOOKMARK_PAGE_FORMAT), static_cast<int>(position.page + 1));
  // pageCount=0 disables EPUB's page-slice deduplication; exact manga anchors
  // above distinguish an overview and every panel on the same physical page.
  // Preserve distinct panel identities in BookmarkStore's move/merge path too,
  // which compares spine and fractional progress (not the paragraph anchor).
  const float fraction = (static_cast<float>(position.page) + static_cast<float>(anchor) / 256.0f) / book.pageCount();
  BOOKMARKS.addBookmark(static_cast<uint16_t>(position.page), fraction, 0, label, anchor);
}

void MangaReaderActivity::handleMenuAction(const MenuAction action) {
  autoTurn.cancel();
  switch (action) {
    case MenuAction::Chapter:
      showSelection(false);
      break;
    case MenuAction::Percent: {
      auto activity = makeUniqueNoThrow<EpubReaderPercentSelectionActivity>(
          renderer, mappedInput, static_cast<int>(position.page * 100 / book.pageCount()));
      if (!activity) {
        LOG_ERR("MANGA", "Cannot allocate percent selector");
        resumeReadingStatsTimer();
        break;
      }
      childActive = true;
      autoTurn.cancel();
      startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (const auto* target = std::get_if<PercentResult>(&result.data)) {
            jump(std::min(book.pageCount() - 1, book.pageCount() * std::clamp(target->percent, 0, 100) / 100));
          }
        }
        childReturned();
      });
      break;
    }
    case MenuAction::Bookmarks:
      showSelection(true);
      break;
    case MenuAction::ToggleBookmark:
      toggleBookmark();
      break;
    case MenuAction::PanelsOnly: {
      RenderLock lock(*this);
      progress.panelsOnly = !progress.panelsOnly;
      position = manga::normalizePosition(position, available, progress.panelsOnly);
      observeProgressLocked();
      break;
    }
    case MenuAction::PanelRotation: {
      RenderLock lock(*this);
      progress.rotatePanels = !progress.rotatePanels;
      observeProgressLocked();
      break;
    }
    case MenuAction::Orientation:
      handleTwoFingerRotation(true);
      break;
    case MenuAction::Home:
      onGoHome();
      return;
    case MenuAction::Lookup:
      openLookup();
      return;
    case MenuAction::Translation:
      openTranslation();
      return;
    case MenuAction::LookupHistory:
      openLookupHistory();
      return;
    case MenuAction::ReaderSettings:
      openMangaSettings();
      return;
    case MenuAction::AutoTurn:
      openAutoTurnMenu();
      return;
    case MenuAction::Screenshot: {
      RenderLock lock(*this);
      pendingScreenshot = true;
      imageDirty = true;
      break;
    }
    case MenuAction::DeleteCache:
      confirmCacheDelete();
      return;
    case MenuAction::OcrQr:
      openQr();
      return;
    case MenuAction::Dismiss:
      if (leaveAfterMessage) {
        leaveAfterMessage = false;
        onGoHome();
        return;
      }
      break;
    case MenuAction::None:
      return;
  }
  if (action != MenuAction::Chapter && action != MenuAction::Percent && action != MenuAction::Bookmarks)
    resumeReadingStatsTimer();
  requestUpdate();
}

void MangaReaderActivity::openMangaSettings() {
  auto activity = makeUniqueNoThrow<ReaderOptionsActivity>(renderer, mappedInput, ReaderSettingsScope::Manga);
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate manga settings");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { childReturned(); });
}

void MangaReaderActivity::openAutoTurnMenu() {
  RenderLock lock(*this);
  const char* rates[] = {tr(STR_OFF), tr(STR_MANGA_AUTO_ONE), tr(STR_MANGA_AUTO_THREE), tr(STR_MANGA_AUTO_SIX),
                         tr(STR_MANGA_AUTO_TWELVE)};
  menu.show(tr(STR_AUTO_TURN_ENABLED), rates, 5, autoTurn.rateIndex(), [this](int choice) {
    pendingInput = {};
    pendingBack = pendingLookup = false;
    autoTurn.select(choice, millis());
    // The popup's normal dismissal path restores BW before the next deadline.
    pendingMenuAction = MenuAction::None;
    imageDirty = true;
  });
  requestUpdate();
}

void MangaReaderActivity::openQr() {
  QrUtils::OwnedPayload payload;
  manga::QrPayloadResult result;
  {
    RenderLock lock(*this);
    if (!foregroundReadyLocked()) return;
    pauseReadingStatsTimer();
    result = manga::buildQrPayload(page, position.panel, payload);
  }
  if (result != manga::QrPayloadResult::Ready) {
    showLookupMessage(I18n::getInstance().get(result == manga::QrPayloadResult::Empty ? StrId::STR_MANGA_NO_OCR
                                              : result == manga::QrPayloadResult::OutOfMemory
                                                  ? StrId::STR_MEMORY_ERROR
                                                  : StrId::STR_PAGE_LOAD_ERROR));
    return;
  }
  std::unique_ptr<QrDisplayActivity> activity;
#ifdef SIMULATOR
  // Targeted allocator failure at the real child-ownership boundary.
  if (!QrDisplayActivity::simulatorTakeActivityFailure())
#endif
    activity = makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, std::move(payload));
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate QR activity");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { childReturned(); });
}

void MangaReaderActivity::confirmCacheDelete() {
  auto activity = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, std::string(tr(STR_DELETE_CACHE)) + "?", bookmarkMetadataLabel(book.title()), false, true);
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate cache confirmation");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
    childReturned();
    if (!result.isCancelled) {
      RenderLock lock(*this);
      pauseReadingStatsTimer();
      pendingCacheDelete = true;
      incrementalStats = true;
      foregroundReadyLocked();
    }
  });
}

void MangaReaderActivity::deleteCacheWhenReady() {
  bool saved = false, deleted = false;
  {
    RenderLock lock(*this);
    if (!foregroundReadyLocked()) return;
    pendingCacheDelete = false;
    pauseReadingStatsTimer();
    saved = saveDurableStateLocked();
    suspensionPersistenceFailed = !saved;
    if (saved) {
      pixelCache.close();
      pixelCache.discardTemporary();
      const auto current = position;
      book.close();
      ready = pixelsReady = false;
      page = {};
      deleted = clearBookCachePreservingUserState(folder);
      if (!deleted) {
        LOG_ERR("MANGA", "Could not clear manga cache");
        ready = book.open(folder.c_str());
        if (ready) {
          loadPageLocked(current.page);
          position = current;
        } else
          LOG_ERR("MANGA", "Could not reopen manga after failed cache clear");
      }
      imageDirty = true;
    } else
      LOG_ERR("MANGA", "Cache retained because durable state could not be saved");
    leaveAfterMessage = deleted;
  }
  showLookupMessage(I18n::getInstance().get(!saved    ? StrId::STR_STATS_SAVE_FAILED
                                            : deleted ? StrId::STR_BOOK_CACHE_DELETED
                                                      : StrId::STR_CLEAR_CACHE_FAILED));
}

bool MangaReaderActivity::queueShortcut(const MenuAction action) {
  RenderLock lock(*this);
  if (!ready || menu.isActive() || inputLocked || suspended || childActive || pendingInput.hasMenu() ||
      pendingCacheDelete || pendingFeedback != StrId::STR_NONE_OPT || foregroundDraining)
    return false;
  autoTurn.cancel();
  if (action == MenuAction::Lookup) {
    pendingLookup = true;
    foregroundReadyLocked();
  } else if (action == MenuAction::Screenshot) {
    pendingScreenshot = true;
    imageDirty = true;
    requestUpdate();
  } else
    return false;
  return true;
}

bool MangaReaderActivity::handleShortcutAction(const uint8_t action) {
  if (action == CrossPointSettings::LOOKUP_WORD) return queueShortcut(MenuAction::Lookup);
  if (action == CrossPointSettings::SCREENSHOT) return queueShortcut(MenuAction::Screenshot);
  return false;
}

bool MangaReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD) return queueShortcut(MenuAction::Lookup);
  if (action == CrossPointSettings::SHORT_PWRBTN::SCREENSHOT) return queueShortcut(MenuAction::Screenshot);
  return false;
}

ScreenshotInfo MangaReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Manga;
  const auto title = book.title();
  const size_t count = std::min(title.size(), sizeof(info.title) - 1);
  const size_t safe = count < title.size() ? utf8SafeTruncateBuffer(title.data(), count) : count;
  if (safe) std::memcpy(info.title, title.data(), safe);
  info.currentPage = static_cast<int>(position.page + 1);
  info.totalPages = static_cast<int>(book.pageCount());
  info.progressPercent = book.pageCount() ? static_cast<int>(position.page * 100 / book.pageCount()) : 0;
  return info;
}

void MangaReaderActivity::drawStatusLocked(const bool grayMask) {
  const auto base = renderer.getOrientation();
  renderer.setOrientation(imageOrientation);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  char counter[48];
  if (position.panel < 0)
    snprintf(counter, sizeof(counter), "%lu/%lu", static_cast<unsigned long>(position.page + 1),
             static_cast<unsigned long>(book.pageCount()));
  else
    snprintf(counter, sizeof(counter), "%d/%u  %lu/%lu", position.panel + 1, available.panelCount,
             static_cast<unsigned long>(position.page + 1), static_cast<unsigned long>(book.pageCount()));
  const char* hint = tr(STR_MANGA_PANELS_HINT);
  const auto layout = manga::layoutStatus(
      left, top, renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom,
      renderer.getTextWidth(UI_10_FONT_ID, counter), renderer.getTextWidth(UI_10_FONT_ID, hint),
      renderer.getLineHeight(UI_10_FONT_ID), position.panel >= 0);
  manga::drawStatus(renderer, layout, UI_10_FONT_ID, counter, hint, grayMask);
  renderer.setOrientation(base);
}

void MangaReaderActivity::showLookupMessage(const char* message) {
  RenderLock lock(*this);
  const char* options[] = {tr(STR_BACK)};
  menu.show(message, options, 1, 0, [this](int) { pendingMenuAction = MenuAction::Dismiss; });
  requestUpdate();
}

void MangaReaderActivity::childReturned() {
  RenderLock lock(*this);
  imageDirty = true;
  suspended = false;
  childActive = false;
  renderedAtMs = 0;
  resumeReadingStatsTimer();
  requestUpdate();
}

void MangaReaderActivity::lookupBackgroundLocked(PageTextSourceView source) {
  // Called synchronously under the child's RenderLock. Parent position is never
  // parked at overview during the child lifetime, including forced teardown.
  const auto savedPosition = position;
  const auto savedOrientation = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!lookupGeometry.textOnly || lookupTextPopup) {
    position.panel = -1;
    if (!drawImageLocked(lookupTextPopup ? nullptr : &lookupGeometry.views)) {
      LOG_ERR("MANGA", "Cannot redraw OCR overview background");
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR));
    }
    pixelCache.close();
  }
  if (lookupGeometry.textOnly) {
    if (lookupTextPopup) {
      const auto& box = lookupGeometry.views.base;
      renderer.fillRect(box.x - 4, box.y - 4, box.width + 8, box.height + 8, false);
      renderer.drawRect(box.x - 4, box.y - 4, box.width + 8, box.height + 8);
    }
    // The child passes its live view only for this call; no borrowed glyph
    // pointer survives source release or a dictionary change.
    for (uint16_t i = 0; i < source.glyphCount; ++i) {
      const auto& glyph = source.glyphs[i];
      if (glyph.pageWord == PageTextGlyph::kSyntheticPageWord || glyph.width <= 0 || glyph.height <= 0) continue;
      PageTextBounds clip{glyph.x, glyph.y, glyph.width, glyph.height};
      if (lookupTextPopup) {
        const auto& box = lookupGeometry.views.base;
        clip = clipPageTextBounds(clip, {int16_t(box.x), int16_t(box.y), int16_t(box.width), int16_t(box.height)});
        if (clip.width <= 0 || clip.height <= 0) continue;
      }
      const uint32_t cp = glyph.codepoint;
      char text[5]{};
      if (cp < 0x80)
        text[0] = static_cast<char>(cp);
      else if (cp < 0x800) {
        text[0] = 0xc0 | (cp >> 6);
        text[1] = 0x80 | (cp & 63);
      } else if (cp < 0x10000) {
        text[0] = 0xe0 | (cp >> 12);
        text[1] = 0x80 | ((cp >> 6) & 63);
        text[2] = 0x80 | (cp & 63);
      } else {
        text[0] = 0xf0 | (cp >> 18);
        text[1] = 0x80 | ((cp >> 12) & 63);
        text[2] = 0x80 | ((cp >> 6) & 63);
        text[3] = 0x80 | (cp & 63);
      }
      renderer.beginTextClip(clip.x, clip.y, clip.width, clip.height);
      renderer.drawText(SETTINGS.getReaderFontId(), glyph.x, glyph.y, text);
      renderer.endTextClip();
    }
  }
  position = savedPosition;
  renderer.setOrientation(savedOrientation);
  imageDirty = true;
}

void MangaReaderActivity::openLookup(const int region) {
  OwnedLookupTextSource source;
  std::unique_ptr<MangaRegionSelectionActivity> selection;
  DictionaryStatus status;
  const auto scope = position;
  {
    RenderLock lock(*this);
    // Menu opening already drained prefetch. Keep it parked through construction.
    if (!foregroundReadyLocked()) {
      LOG_ERR("MANGA", "Lookup requested before prefetch drained");
      return;
    }
    pauseReadingStatsTimer();
    sdFontSystem.ensureLoaded(renderer);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    captureViewportsLocked();
    lookupGeometry = {};
    lookupTextPopup = false;
    lookupGeometry.views = viewports;
    manga::format::IndexRecord info;
    const bool metadataReady = book.readPageInfo(scope.page, info);
    lookupGeometry.sourceWidth = info.imageWidth;
    lookupGeometry.sourceHeight = info.imageHeight;
    lookupGeometry.textOnly = !available.overview || !metadataReady || !info.imageWidth || !info.imageHeight;
    if (!lookupGeometry.textOnly) {
      if (region < 0) {
        const auto bar = MangaRegionSelectionActivity::toolbar(renderer);
        auto& viewport = lookupGeometry.views.base;
        viewport.height = std::min(viewport.height, std::max(1, bar.y - 4 - viewport.y));
        lookupGeometry.views.rotated = {viewport.y, lookupGeometry.views.screenWidth - viewport.x - viewport.width,
                                        viewport.height, viewport.width};
      }
      position.panel = -1;
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      const bool drawn = drawImageLocked(&lookupGeometry.views);
      position = scope;
      pixelCache.close();
      lookupGeometry.textOnly = !drawn;
      lookupGeometry.layout.geometry = imageGeometry;
      lookupGeometry.layout.orientation = static_cast<int>(imageOrientation);
      lookupGeometry.layout.screenWidth =
          imageOrientation == renderer.getOrientation() ? viewports.screenWidth : viewports.screenHeight;
      lookupGeometry.layout.screenHeight =
          imageOrientation == renderer.getOrientation() ? viewports.screenHeight : viewports.screenWidth;
    }
    if (region >= 0) {
      lookupTextPopup = true;
      lookupGeometry.textOnly = true;
      const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      lookupGeometry.views.base = {safe.x + 8, safe.y + 8, std::max(1, safe.width - 16),
                                   std::max(1, safe.height / 4 - 16)};
    }
    if (lookupGeometry.textOnly) {
      lookupGeometry.lineHeight = std::max(1, renderer.getLineHeight(SETTINGS.getReaderFontId()));
      lookupGeometry.cellWidth =
          std::max(lookupGeometry.lineHeight, renderer.getTextWidth(SETTINGS.getReaderFontId(), "W"));
      if (!lookupTextPopup) lookupGeometry.views.base.height /= 2;
    }
    if (region < 0 && !lookupGeometry.textOnly) {
      const int first = nextMangaLookupRegion(page, scope.panel, lookupGeometry, -1, true);
      status = first < 0 ? DictionaryStatus::NotFound : DictionaryStatus::Found;
      if (first >= 0) {
        selection = makeUniqueNoThrow<MangaRegionSelectionActivity>(
            renderer, mappedInput, page, scope.panel, lookupGeometry, first,
            [](void* parent) { static_cast<MangaReaderActivity*>(parent)->lookupBackgroundLocked({}); }, this);
        if (!selection) {
          LOG_ERR("MANGA", "Cannot allocate OCR region selection (%u bytes)",
                  unsigned(sizeof(MangaRegionSelectionActivity)));
          status = DictionaryStatus::OutOfMemory;
        }
      }
    } else {
      status = buildMangaLookupTextSource(page, scope.panel, lookupGeometry, source, region);
    }
    imageDirty = true;
  }
  if (status != DictionaryStatus::Found) {
    if (status == DictionaryStatus::NotFound)
      LOG_INF("MANGA", "No OCR text page=%lu panel=%d", static_cast<unsigned long>(scope.page), scope.panel);
    else
      LOG_ERR("MANGA", "OCR source unavailable page=%lu panel=%d status=%u", static_cast<unsigned long>(scope.page),
              scope.panel, static_cast<unsigned>(status));
    showLookupMessage(status == DictionaryStatus::NotFound      ? tr(STR_MANGA_NO_OCR)
                      : status == DictionaryStatus::OutOfMemory ? tr(STR_MEMORY_ERROR)
                                                                : tr(STR_PAGE_LOAD_ERROR));
    return;
  }
  if (selection) {
    childActive = true;
    autoTurn.cancel();
    startActivityForResult(std::move(selection), [this](const ActivityResult& result) {
      childReturned();
      const auto* selected = std::get_if<MenuResult>(&result.data);
      if (!result.isCancelled && selected && selected->action >= 0) openLookup(selected->action);
    });
    return;
  }
  EpubLookupPageRequest request;
  if (lookupTextPopup) {
    const auto& box = lookupGeometry.views.base;
    request.externalTextViewport = {int16_t(box.x), int16_t(box.y), int16_t(box.width), int16_t(box.height)};
  }
  request.bookLanguage = bookmarkMetadataLabel(book.language());
  request.bookCachePath = statsCachePath;
  request.spineIndex = static_cast<uint16_t>(scope.page);
  request.pageIndex = static_cast<uint16_t>(scope.panel + 1);
  char leaf[64];
  if (!statsCachePath.empty()) {
    if (region >= 0) {
      snprintf(leaf, sizeof(leaf), "ocr_%lu_%d_region%d.scan", static_cast<unsigned long>(scope.page), scope.panel,
               region);
      request.scanCacheFilePath = statsCachePath + "/" + leaf;
    } else if (mangaLookupCacheFileName(scope.page, scope.panel, leaf, sizeof(leaf))) {
      request.scanCacheFilePath = statsCachePath + "/" + leaf;
    }
  }
  request.dictionaryFontFamilyName = SETTINGS.dictionarySdFontFamilyName;
  request.dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
  request.readerContext = this;
  request.renderExternalBackground = [](void* parent, PageTextSourceView view) {
    static_cast<MangaReaderActivity*>(parent)->lookupBackgroundLocked(view);
  };
  auto activity =
      makeUniqueNoThrow<EpubReaderWordLookupActivity>(renderer, mappedInput, std::move(source), std::move(request));
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate shared lookup activity");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  autoTurn.cancel();
  startActivityForResult(std::move(activity), [this, scope](const ActivityResult& result) {
    childReturned();
    if (result.isCancelled) return;
    const auto* clip = std::get_if<DictionaryClippingRequest>(&result.data);
    if (!clip) return;
    // At most 1024 source codepoints. Temporary 4097-byte exact UTF-8 buffer
    // exceeds the C3 stack budget; release it immediately after saving.
    auto text = makeUniqueNoThrow<char[]>(kMangaLookupMaxGlyphs * 4 + 1);
    size_t length = 0;
    const MangaLookupClippingRange range{clip->firstPageWordOrdinal, clip->lastPageWordOrdinal,
                                         clip->firstWordByteOffset, clip->lastWordByteEndOffset};
    bool saved = false;
    if (text && scope.page == position.page &&
        copyMangaLookupClipping(page, scope.panel, range, text.get(), kMangaLookupMaxGlyphs * 4 + 1, length)) {
      char context[96];
      if (scope.panel < 0)
        snprintf(context, sizeof(context), tr(STR_MANGA_CLIP_PAGE), static_cast<unsigned long>(scope.page + 1));
      else
        snprintf(context, sizeof(context), tr(STR_MANGA_CLIP_PANEL), static_cast<unsigned long>(scope.page + 1),
                 scope.panel + 1);
      saved =
          ClippingsManager::saveClipping(bookmarkMetadataLabel(book.title()), bookmarkMetadataLabel(book.author()),
                                         context, static_cast<int>(scope.page + 1), std::string(text.get(), length));
    }
    if (!saved) LOG_ERR("MANGA", "Could not reconstruct or save manga clipping");
    showLookupMessage(saved ? tr(STR_CLIPPING_SAVED) : tr(STR_CLIPPING_FAILED));
  });
}

void MangaReaderActivity::openTranslation() {
  auto activity = makeUniqueNoThrow<MangaTranslationActivity>(renderer, mappedInput, page, position.panel);
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate translation activity");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  autoTurn.cancel();
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { childReturned(); });
}

void MangaReaderActivity::openLookupHistory() {
  auto activity = makeUniqueNoThrow<LookedUpWordsActivity>(
      renderer, mappedInput, bookmarkMetadataLabel(book.language()), statsCachePath,
      SETTINGS.dictionarySdFontFamilyName, SETTINGS.dictionaryFontPointSize);
  if (!activity) {
    LOG_ERR("MANGA", "Cannot allocate lookup history");
    showLookupMessage(tr(STR_MEMORY_ERROR));
    return;
  }
  childActive = true;
  autoTurn.cancel();
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { childReturned(); });
}

void MangaReaderActivity::onInputLockChanged(const bool locked) {
  RenderLock lock(*this);
  inputLocked = locked;
  if (locked) autoTurn.cancel();
  if (locked)
    pauseReadingStatsTimer();
  else
    resumeReadingStatsTimer();
}

bool MangaReaderActivity::handleTwoFingerRotation(bool clockwise) {
  RenderLock lock(*this);
  if (!ready || menu.isActive() || inputLocked || suspended || autoTurn.active()) return false;
  pendingInput.rotate(clockwise);
  foregroundReadyLocked();
  return true;
}

bool MangaReaderActivity::prepareManualRefresh() {
  // ActivityManager::requestManualReaderRefresh already owns RenderLock.
  foregroundReadyLocked();
  imageDirty = true;
  refreshCountdown = -1;
  return true;
}

void MangaReaderActivity::loop() {
  StrId feedback;
  {
    RenderLock lock(*this);
    feedback = pendingFeedback;
    pendingFeedback = StrId::STR_NONE_OPT;
  }
  if (feedback != StrId::STR_NONE_OPT) {
    showLookupMessage(I18n::getInstance().get(feedback));
    return;
  }
  if (pendingCacheDelete) {
    deleteCacheWhenReady();
    return;
  }
  MenuAction action = MenuAction::None;
  {
    RenderLock lock(*this);
    pollPrefetchLocked();
    if (pendingLookup && (!prefetch || prefetch->idle())) {
      pendingLookup = false;
      action = MenuAction::Lookup;
    }
    // Open before routing this frame's menu press. The release that requested
    // the menu has already been consumed; a new Confirm press must select it.
    if (action == MenuAction::None && (!prefetch || prefetch->idle()) && pendingInput.takeMenu()) showMenuLocked();
    if (action == MenuAction::None && menu.isActive()) {
      menu.handleInput(mappedInput, [this] { requestUpdate(); });
      if (!menu.isActive()) {
        imageDirty = true;
        if (leaveAfterMessage) pendingMenuAction = MenuAction::Dismiss;
      }
      action = pendingMenuAction;
      pendingMenuAction = MenuAction::None;
      if (action == MenuAction::None) {
        if (!menu.isActive()) resumeReadingStatsTimer();
        return;
      }
    }
    if (ready && (!prefetch || prefetch->idle())) observeProgressLocked();
  }
  if (action != MenuAction::None) {
    handleMenuAction(action);
    return;
  }
  {
    RenderLock lock(*this);
    if (autoTurn.active()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          mappedInput.wasReleased(MappedInputManager::Button::Back) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
        autoTurn.cancel();
        imageDirty = true;
        requestUpdate();
        return;
      }
      // Automatic mode owns input, but cancellation must consume its edge
      // before a deferred refresh can return from this iteration.
      if (pendingRender && (!prefetch || prefetch->idle())) {
        pendingRender = false;
        requestUpdate();
        return;
      }
      const bool idle =
          !imageDirty && !pendingRender && !pendingScreenshot && !suspended && (!prefetch || prefetch->idle());
      if (autoTurn.poll(millis(), idle)) {
        const auto target = manga::automaticNext(position, book.pageCount(), available, progress.panelsOnly);
        applyMoveLocked(target, true);
        if (!target.changed) autoTurn.cancel();
      }
      return;
    }
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    mappedInput.suppressNextBackRelease();
    activityManager.goToFileBrowser(FsHelpers::extractFolderPath(folder));
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    bool overview = false;
    {
      RenderLock lock(*this);
      pendingLookup = false;
      overview = ready && position.panel >= 0 && available.overview && !progress.panelsOnly;
      if (overview && !foregroundReadyLocked()) {
        pendingBack = true;
        return;
      }
      if (overview) {
        position.panel = -1;
        imageDirty = true;
        observeProgressLocked();
      }
    }
    if (overview)
      requestUpdate();
    else
      onGoHome();
    return;
  }
  if (!ready) return;
  if (ReaderUtils::isTouchMenuGesture(mappedInput)) {
    openReaderSettingsMenu();
    return;
  }
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (longPressMenuHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressMenuHandled = false;
    return;
  }
  if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_LOOKUP_WORD &&
      (mappedInput.isPressed(MappedInputManager::Button::Confirm) || confirmReleased) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    if (queueShortcut(MenuAction::Lookup)) {
      longPressMenuHandled = !confirmReleased;
      if (!confirmReleased) mappedInput.suppressNextConfirmRelease();
    }
    return;
  }
  if (confirmReleased) {
    bool panelLookup = false;
    {
      RenderLock lock(*this);
      panelLookup = position.panel >= 0;
      if (panelLookup) {
        pendingInput.takeMenu();
        pendingLookup = true;  // Repeated Confirm edges coalesce while prefetch drains.
        foregroundReadyLocked();
      }
    }
    if (!panelLookup) openReaderSettingsMenu();
    return;
  }
  ReaderUtils::TouchPageTurn touch{};
  {
    RenderLock lock(*this);
    touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  }
  if (touch.prev || mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left))
    move(false);
  else if (touch.next || mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
           mappedInput.wasReleased(MappedInputManager::Button::Right))
    move(true);
  // Consume retained intent only after worker cleanup. Polling input above
  // continues while cancellation drains; repeated directions coalesce.
  bool drain = false;
  {
    RenderLock lock(*this);
    drain = !prefetch || prefetch->idle();
  }
  if (!drain) return;
  {
    RenderLock lock(*this);
    const int rotations = pendingInput.takeRotations();
    if (rotations) {
      for (int i = 0; i < rotations; ++i)
        SETTINGS.orientation = ReaderUtils::rotatedOrientation(SETTINGS.orientation, true);
      SETTINGS.saveToFile();
      imageDirty = true;
      refreshCountdown = 0;
      requestUpdate();
    }
  }
  if (pendingBack) {
    RenderLock lock(*this);
    pendingBack = false;
    position.panel = -1;
    imageDirty = true;
    observeProgressLocked();
    requestUpdate();
    return;
  }
  RenderLock lock(*this);
  const int direction = pendingInput.takeMove();
  if (direction) {
    moveLocked(direction > 0);
    return;
  }
  // Capture and consume manual intent first, including an edge arriving in
  // the same iteration as worker completion. Rendering must not discard it.
  if (pendingRender) {
    pendingRender = false;
    requestUpdate();
    return;
  }
  warmLocked();
}

void MangaReaderActivity::pauseReadingStatsTimer() {
  recordCurrentPageReadingTime();
  pageShownAtMs = 0UL;
}

void MangaReaderActivity::resumeReadingStatsTimer() {
  pageShownAtMs = ready && !menu.isActive() && !inputLocked && !suspended && !childActive ? millis() : 0UL;
}

bool MangaReaderActivity::elapsedPageReadingMs(uint32_t& elapsed) const {
  elapsed = 0;
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) return false;
  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs / 1000UL > SETTINGS.getReadingIdleTimeThresholdSeconds()) return false;
  elapsed = static_cast<uint32_t>(elapsedMs);
  return elapsed != 0;
}

void MangaReaderActivity::recordCurrentPageReadingTime() {
  uint32_t elapsed = 0;
  if (elapsedPageReadingMs(elapsed)) {
    // Keep partial seconds across panel changes and menu pauses. Convert only at
    // the physical-page/stat-store boundary so several short views still count.
    sessionReadingMs = sessionReadingMs > UINT32_MAX - elapsed ? UINT32_MAX : sessionReadingMs + elapsed;
    physicalPageReadingMs = physicalPageReadingMs > UINT32_MAX - elapsed ? UINT32_MAX : physicalPageReadingMs + elapsed;
  }
  pageShownAtMs = 0UL;
}

void MangaReaderActivity::recordForwardPageTurn(const uint32_t seconds) {
  stats.recordForwardPageRead(seconds);
  if (stats.totalPagesTurned < UINT32_MAX) stats.totalPagesTurned++;
  if (globalStats.totalPagesTurned < UINT32_MAX) globalStats.totalPagesTurned++;
  statsCommit.changed();
}

bool MangaReaderActivity::saveDurableStateLocked() {
  return (!saveDebouncer.hasPending() || saveProgressLocked()) && commitReadingStats() && BOOKMARKS.saveToFileChecked();
}

bool MangaReaderActivity::commitReadingStats() {
  if (!ready || statsCachePath.empty()) return !statsCommit.pending();
  recordCurrentPageReadingTime();
  const auto start = hasSessionStartLocalDateTime ? sessionStartLocalDateTime : ReadingStatsDateTime{};
  return statsCommit.flush(statsCachePath, stats, globalStats, sessionReadingMs, start, book.language());
}

void MangaReaderActivity::setBookCompleted(const bool completed) {
  if (stats.isCompleted == completed || statsCachePath.empty()) return;
  stats.isCompleted = completed;
  if (completed && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) stats.finishedDate = now.date;
  }
  if (completed) {
    if (globalStats.completedBooks < UINT32_MAX) globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }
  statsCommit.changed();
  if (!statsCommit.pending()) commitReadingStats();
}

bool MangaReaderActivity::drawCachedPixelsLocked() {
  if (!pixelScratch || !pixelCache.rewind()) return false;
  DirectPixelWriter writer;
  writer.init(renderer);
  uint32_t lastYield = millis();
  for (int y = 0; y < imageGeometry.height; ++y) {
    if (!pixelCache.readRow(pixelScratch.get(), manga::kBitmapPixelScratchBytes)) return false;
    writer.beginRow(imageGeometry.y + y);
    for (int x = 0; x < imageGeometry.width; ++x) {
      const uint8_t level = (pixelScratch[x / 4] >> (6 - (x % 4) * 2)) & 3;
      writer.writePixel(imageGeometry.x + x, level);
    }
    ImageToFramebufferDecoder::yieldDuringDecode(lastYield);
  }
  return true;
}

bool MangaReaderActivity::drawImageLocked(const manga::ImageViewports* lookupViews) {
  pixelCache.close();
  pixelsReady = false;
  const auto found = position.panel < 0 ? book.pageImagePath(position.page, path.get(), pathCapacity)
                                        : book.panelImagePath(position.page, position.panel, path.get(), pathCapacity);
  if (found != manga::PathResult::Found) return false;
  imagePath.assign(path.get());
  const bool bmp = FsHelpers::hasBmpExtension(imagePath);
  bool bitmapProbed = false;
  bool monochrome = false;
  ImageDimensions dimensions{};
  manga::PixelIdentity cachedSource{};
  manga::PixelIdentity sourceFingerprint{};
  bool sourceFingerprintAttempted = false;
  bool sourceFingerprintReady = false;
  const auto fingerprintSource = [&]() {
    if (!sourceFingerprintAttempted) {
      sourceFingerprintAttempted = true;
      sourceFingerprintReady = manga::fingerprintImage(imagePath.c_str(), sourceFingerprint);
    }
  };
  const bool cachedMetadataReady =
      manga::MangaPixelCache::sourceIdentity(folder, position.page, position.panel, cachedSource);
  if (cachedMetadataReady) fingerprintSource();
  const bool cachedSourceReady =
      sourceFingerprintReady && cachedSource.sourcePathCrc == sourceFingerprint.sourcePathCrc &&
      cachedSource.sourceCrc == sourceFingerprint.sourceCrc && cachedSource.sourceSize == sourceFingerprint.sourceSize;
  if (cachedSourceReady) {
    dimensions.width = static_cast<int16_t>(cachedSource.sourceWidth);
    dimensions.height = static_cast<int16_t>(cachedSource.sourceHeight);
    LOG_DBG("MANGA", "Pixel cache source geometry hit: page %lu panel %d", static_cast<unsigned long>(position.page),
            position.panel);
  }
  ImageToFramebufferDecoder* decoder = nullptr;
  if (bmp) {
    if (!cachedSourceReady) {
      manga::BitmapPixelInfo info;
      if (!manga::probeBitmapPixels(imagePath.c_str(), info)) {
        LOG_ERR("MANGA", "Invalid BMP: %s", path.get());
        return false;
      }
      dimensions.width = static_cast<int16_t>(info.width);
      dimensions.height = static_cast<int16_t>(info.height);
      monochrome = info.bitsPerPixel == 1;
      bitmapProbed = true;
    }
  } else {
    decoder = ImageDecoderFactory::getDecoder(imagePath);
    if (!decoder || (!cachedSourceReady && !decoder->getDimensions(imagePath, dimensions))) return false;
  }
  const auto base = renderer.getOrientation();
  captureViewportsLocked();
  if (lookupViews) viewports = *lookupViews;
  manga::ImageLayout layout;
  if (!manga::buildImageLayout(dimensions.width, dimensions.height, viewports,
                               position.panel < 0 || progress.rotatePanels, bmp, layout))
    return false;
  imageGeometry = layout.geometry;
  imageOrientation = static_cast<GfxRenderer::Orientation>(layout.orientation);
  renderer.setOrientation(imageOrientation);
  const bool success = [&]() {
    manga::PixelIdentity geometryIdentity;
    manga::applyImageLayout(layout, dimensions.width, dimensions.height, bmp, geometryIdentity, imageConfig);
    imageConfig.cachePath.clear();

    bool cacheConfigured = false;
    if (!monochrome && pixelScratch) {
      fingerprintSource();
      manga::PixelIdentity identity = sourceFingerprint;
      manga::applyImageLayout(layout, dimensions.width, dimensions.height, bmp, identity, imageConfig);
      cacheConfigured = sourceFingerprintReady && pixelCache.configure(folder, position.page, position.panel, identity);
      if (cacheConfigured && pixelCache.open()) {
        if (drawCachedPixelsLocked()) {
          pixelsReady = true;
          LOG_DBG("MANGA", "Pixel cache hit: page %lu panel %d", static_cast<unsigned long>(position.page),
                  position.panel);
          return true;
        }
        pixelCache.close();
        renderer.clearScreen();
      }
      if (cacheConfigured) {
        pixelCache.discardTemporary();
        imageConfig.cachePath.assign(pixelCache.temporaryPath());
      }
    }

    if (bmp) {
      if (cacheConfigured &&
          manga::writeBitmapPixels(imagePath.c_str(), pixelCache.temporaryPath(), imageGeometry.width,
                                   imageGeometry.height, pixelScratch.get(), manga::kBitmapPixelScratchBytes) &&
          pixelCache.publish() && pixelCache.open() && drawCachedPixelsLocked()) {
        pixelsReady = true;
        LOG_DBG("MANGA", "Pixel cache created: page %lu panel %d", static_cast<unsigned long>(position.page),
                position.panel);
        return true;
      }
      pixelCache.close();
      pixelCache.discardTemporary();
      renderer.clearScreen();
      if (!bitmapProbed) {
        manga::BitmapPixelInfo info;
        if (!manga::probeBitmapPixels(imagePath.c_str(), info) || info.width != dimensions.width ||
            info.height != dimensions.height) {
          LOG_ERR("MANGA", "Invalid BMP fallback: %s", path.get());
          return false;
        }
      }
      FsFile file;
      if (!Storage.openFileForRead("MANGA", imagePath, file)) return false;
      Bitmap bitmap(file, false);
      bool drawn = bitmap.parseHeaders() == BmpReaderError::Ok;
      if (drawn)
        renderer.drawBitmap(bitmap, imageGeometry.x, imageGeometry.y, imageGeometry.width, imageGeometry.height);
      const bool closed = file.close();
      return drawn && closed;
    }

    const bool decoded = decoder->decodeToFramebuffer(imagePath, renderer, imageConfig);
    if (decoded && cacheConfigured && pixelCache.publish() && pixelCache.open()) {
      // Replay the just-published pixels too: cold and warm BW use exactly the
      // same mapping, and grayscale never needs another JPEG/PNG decode.
      renderer.clearScreen();
      pixelsReady = drawCachedPixelsLocked();
      if (!pixelsReady) {
        pixelCache.close();
        imageConfig.cachePath.clear();
        renderer.clearScreen();
        return decoder->decodeToFramebuffer(imagePath, renderer, imageConfig);
      }
      LOG_DBG("MANGA", "Pixel cache created: page %lu panel %d", static_cast<unsigned long>(position.page),
              position.panel);
    } else {
      pixelCache.close();
      pixelCache.discardTemporary();
    }
    return decoded;
  }();
  renderer.setOrientation(base);
  return success;
}

bool MangaReaderActivity::displayImageGrayscaleLocked() {
  const auto base = renderer.getOrientation();
  renderer.setOrientation(imageOrientation);
  drawStatusLocked();
#ifdef SIMULATOR
  logMangaFramebufferHash(renderer, position.page, position.panel, imageOrientation, "BW");
#endif
  ReaderUtils::displayWithRefreshCycle(renderer, refreshCountdown, false, true);
  renderer.preconditionGrayscale(imageGeometry.x, imageGeometry.y, imageGeometry.width, imageGeometry.height);
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  bool complete = drawCachedPixelsLocked();
  if (complete) {
    drawStatusLocked(true);
#ifdef SIMULATOR
    logMangaFramebufferHash(renderer, position.page, position.panel, imageOrientation, "LSB");
#endif
    renderer.copyGrayscaleLsbBuffers();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    complete = drawCachedPixelsLocked();
    if (complete) {
      drawStatusLocked(true);
#ifdef SIMULATOR
      logMangaFramebufferHash(renderer, position.page, position.panel, imageOrientation, "MSB");
#endif
      renderer.copyGrayscaleMsbBuffers();
      renderer.displayGrayBuffer();
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  bool restored = drawCachedPixelsLocked();
#ifdef SIMULATOR
  const bool forcedRestoreFailure = failNextBwRestoreForTest;
  failNextBwRestoreForTest = false;
  if (forcedRestoreFailure) restored = false;
#endif
  if (restored) drawStatusLocked();
#ifdef SIMULATOR
  if (restored) logMangaFramebufferHash(renderer, position.page, position.panel, imageOrientation, "RESTORED_BW");
#endif
  renderer.setOrientation(base);
  if (!restored) {
    pixelCache.close();
#ifdef SIMULATOR
    restored = !forcedRestoreFailure && drawImageLocked();
#else
    restored = drawImageLocked();
#endif
    if (restored) drawStatusLocked();
  }
  if (!restored) {
    LOG_ERR("MANGA", "Could not restore BW image after grayscale");
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR));
  }
  // Existing renderer API explicitly supports a re-rendered BW baseline. No
  // screen-sized backup is allocated; replay uses the same small row scratch.
  renderer.cleanupGrayscaleWithFrameBuffer();
  if (!complete || !restored) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  pixelCache.close();
  return restored;
}

void MangaReaderActivity::render(RenderLock&&) {
  if (!foregroundReadyLocked()) {
    pendingRender = true;
    return;
  }
  if (suspended) return;
  // Any successful render fulfills a deferred request, including a menu that
  // took priority over the ordinary pending-render service path.
  pendingRender = false;
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  bool frameReady = false;
  if (imageDirty || !menu.isActive()) {
    renderer.clearScreen();
    renderer.setRenderMode(GfxRenderer::BW);
    // The successful cache-delete popup owns a blank background; the book is
    // already closed and must not be reopened or decoded behind that feedback.
    frameReady = !leaveAfterMessage && ready && drawImageLocked();
    if (!frameReady && !leaveAfterMessage) {
#ifdef SIMULATOR
      ++renderErrorsForTest;
#endif
      LOG_ERR("MANGA", "Page %lu panel %d could not be rendered", static_cast<unsigned long>(position.page),
              position.panel);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR));
    }
    imageDirty = false;
  }
  if (menu.processRender(renderer, mappedInput)) {
    pixelCache.close();
    return;
  }
  if (frameReady && pixelsReady) {
    frameReady = displayImageGrayscaleLocked();
  } else {
    if (frameReady) drawStatusLocked();
    ReaderUtils::displayWithRefreshCycle(renderer, refreshCountdown);
  }
  pixelCache.close();
  renderedAtMs = millis();
  autoTurn.rendered(renderedAtMs);
  if (pendingScreenshot) {
    pendingScreenshot = false;
    if (!frameReady)
      pendingFeedback = StrId::STR_PAGE_LOAD_ERROR;
    else if (!ScreenshotUtil::takeScreenshot(renderer))
      pendingFeedback = StrId::STR_MANGA_SCREENSHOT_FAILED;
  }
  prefetchCandidate = 0;
}

void MangaReaderActivity::captureViewportsLocked() {
  const auto base = renderer.getOrientation();
  const auto capture = [&]() {
    int top, right, bottom, left;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    return manga::ImageViewport{left, top, renderer.getScreenWidth() - left - right,
                                renderer.getScreenHeight() - top - bottom};
  };
  viewports.orientation = static_cast<int>(base);
  viewports.screenWidth = renderer.getScreenWidth();
  viewports.screenHeight = renderer.getScreenHeight();
  viewports.base = capture();
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(manga::rotatedImageOrientation(viewports.orientation)));
  viewports.rotated = capture();
  renderer.setOrientation(base);
}
void MangaReaderActivity::pollPrefetchLocked() {
  bool failed = false;
  if (prefetch && prefetch->poll(failed) && failed) prefetchRetryAtMs = millis() + 30000;
  if (!prefetch || prefetch->idle()) foregroundDraining = false;
}
bool MangaReaderActivity::foregroundReadyLocked() {
  renderedAtMs = 0;
  if (!prefetch) return true;
  prefetch->cancel();
  pollPrefetchLocked();
  foregroundDraining = !prefetch->idle();
  return !foregroundDraining;
}
bool MangaReaderActivity::prepareToSuspend() {
  // Caller holds RenderLock. Worker drain and persistence failure are distinct:
  // the former waits, the latter cancels the transition and restores input.
  autoTurn.cancel();
  pauseReadingStatsTimer();
  suspended = true;
  suspensionPersistenceFailed = false;
  if (!foregroundReadyLocked()) return false;
  if (incrementalStats && !saveDurableStateLocked()) {
    suspensionPersistenceFailed = true;
    suspended = false;
    childActive = false;
    pendingFeedback = StrId::STR_STATS_SAVE_FAILED;
    imageDirty = true;
    requestUpdate();
    return false;
  }
  return true;
}

void MangaReaderActivity::onResume() {
  suspended = false;
  childActive = false;
  renderedAtMs = 0;
  resumeReadingStatsTimer();
}

void MangaReaderActivity::warmLocked() {
  if (!prefetch || !prefetch->idle() || !ready || menu.isActive() || imageDirty || suspended || childActive ||
      inputLocked || pendingScreenshot || !renderedAtMs ||
      millis() - renderedAtMs < (prefetchCandidate == 0 ? 150UL : 400UL) ||
      (prefetchRetryAtMs && static_cast<int32_t>(millis() - prefetchRetryAtMs) < 0))
    return;
  // One candidate per loop. The foreground adapter resolves legacy filenames
  // without loading another PageView, and copies the final path before posting.
  uint32_t number = position.page;
  int16_t panel = -1;
  manga::PathResult found = manga::PathResult::Missing;
  if (prefetchCandidate == 0) {
    for (int i = position.panel + 1; i < available.panelCount; ++i) {
      if (available.hasCrop(i)) {
        panel = i;
        break;
      }
    }
    if (panel >= 0) found = book.panelImagePath(number, panel, path.get(), pathCapacity);
  } else if (prefetchCandidate == 1 && number + 1 < book.pageCount()) {
    ++number;
    found = book.pageImagePath(number, path.get(), pathCapacity);
  } else
    return;
  ++prefetchCandidate;
  if (found == manga::PathResult::Found)
    prefetch->post(path.get(), number, panel, panel < 0 || progress.rotatePanels, viewports);
}

#ifdef SIMULATOR
manga::Position MangaReaderActivity::simulatorPosition() {
  RenderLock lock(*this);
  return position;
}
void MangaReaderActivity::simulatorFailNextBwRestore() {
  RenderLock lock(*this);
  failNextBwRestoreForTest = true;
}
bool MangaReaderActivity::simulatorFeedbackIs(const StrId message) {
  RenderLock lock(*this);
  return menu.simulatorTitleIs(I18n::getInstance().get(message));
}
bool MangaReaderActivity::simulatorNoOcrFeedback() {
  RenderLock lock(*this);
  return menu.simulatorTitleIs(tr(STR_MANGA_NO_OCR));
}
bool MangaReaderActivity::simulatorMenuOptionCenter(const int index, int& x, int& y) {
  RenderLock lock(*this);
  return menu.simulatorOptionCenter(renderer, index, x, y);
}
bool MangaReaderActivity::simulatorMenuActive() {
  RenderLock lock(*this);
  return menu.isActive();
}
bool MangaReaderActivity::simulatorJumpWhenIdle(manga::Position target) {
  {
    RenderLock lock(*this);
    if (!foregroundReadyLocked()) return false;
  }
  jump(target.page, target.panel);
  return true;
}
bool MangaReaderActivity::simulatorPendingRender() {
  RenderLock lock(*this);
  return pendingRender;
}
bool MangaReaderActivity::simulatorStartWarmWhenIdle() {
  {
    RenderLock lock(*this);
    if (!foregroundReadyLocked()) return false;
  }
  return simulatorQueueCurrentSource();
}
bool MangaReaderActivity::simulatorQueueCurrentSource() {
  RenderLock lock(*this);
  if (!prefetch) return false;
  pollPrefetchLocked();
  if (!prefetch->idle()) return true;
  const auto found = position.panel < 0 ? book.pageImagePath(position.page, path.get(), pathCapacity)
                                        : book.panelImagePath(position.page, position.panel, path.get(), pathCapacity);
  if (found != manga::PathResult::Found) return false;
  captureViewportsLocked();
  renderedAtMs = 0;
  return prefetch->post(path.get(), position.page, position.panel, position.panel < 0 || progress.rotatePanels,
                        viewports);
}
#endif
