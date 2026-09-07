#include "BookActions.h"

#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstdio>

#include "BookCompletionActivity.h"
#include "BookCompletionEdit.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/MangaProgressStore.h"
#include "activities/reader/ReadingStatsSave.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookFolderMutation.h"
#include "util/BookMoveUtils.h"

namespace BookActions {
namespace {

std::string boundedMetadata(std::string_view value) {
  const int bytes = utf8SafeTruncateBuffer(value.data(), static_cast<int>(std::min<size_t>(value.size(), 127)));
  return bytes > 0 ? std::string(value.data(), static_cast<size_t>(bytes)) : std::string{};
}

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) ||
         manga::MangaBook::isMangaFolder(path.c_str());
}

std::string bookStatsCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (manga::MangaBook::isMangaFolder(path.c_str())) {
    return manga::cachePath(path);
  }
  return "";
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(includeRemoveFromRecents ? 7 : 6);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasAnkiDeckExtension(path) ||
         FsHelpers::hasXtcExtension(path) || manga::MangaBook::isMangaFolder(path.c_str());
}

bool canSendNearby(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasAnkiDeckExtension(path) ||
         FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasPngExtension(path) ||
         FsHelpers::hasBmpExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  if (BookFolderMutation::storesFrozen()) return;
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasAnkiDeckExtension(fullPath)) {
    clearBookCachePreservingUserState(fullPath);
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  } else if (manga::MangaBook::isMangaFolder(fullPath.c_str())) {
    clearMangaMetadata(fullPath);
  }
}

bool clearMangaMetadata(const std::string& folderPath) {
  if (BookFolderMutation::storesFrozen()) return false;
  bool ok = true;
  const std::string cachePath = manga::cachePath(folderPath);
  if (Storage.exists(cachePath.c_str()) && !Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("BookActions", "Failed to remove manga cache: %s", cachePath.c_str());
    ok = false;
  }
  if (!manga::MangaProgressStore(folderPath).remove()) ok = false;
  BookmarkStore::deleteForFilePath(folderPath, "manga");
  return ok;
}

bool clearBookCache(const std::string& fullPath) {
  if (BookFolderMutation::storesFrozen()) return false;
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasAnkiDeckExtension(fullPath) ||
      FsHelpers::hasXtcExtension(fullPath) || manga::MangaBook::isMangaFolder(fullPath.c_str())) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  if (BookFolderMutation::storesFrozen()) return false;
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (BookFolderMutation::storesFrozen()) return false;
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossInkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed,
                         CompletionEdit& edit) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  const bool isManga = manga::MangaBook::isMangaFolder(fullPath.c_str());
  if (!isEpub && !isXtc && !isManga) {
    return false;
  }

  auto& cachePath = edit.cachePath;
  auto& title = edit.title;
  auto& author = edit.author;
  auto& thumbPath = edit.thumbPath;
  auto& stats = edit.book;
  auto& globalStats = edit.global;
  if (!edit.initialized) {
    Epub epub(fullPath, "/.crosspoint");
    Xtc xtc(fullPath, "/.crosspoint");
    manga::MangaBook mangaBook;
    if (isEpub) {
      epub.setupCacheDir();
      cachePath = epub.getCachePath();
      title = epub.getTitle();
      author = epub.getAuthor();
      thumbPath = epub.getThumbBmpPath();
    } else if (isXtc) {
      if (!xtc.load()) {
        return false;
      }
      xtc.setupCacheDir();
      cachePath = xtc.getCachePath();
      title = xtc.getTitle();
      author = xtc.getAuthor();
      thumbPath = xtc.getThumbBmpPath();
    } else {
      if (!mangaBook.open(fullPath.c_str(), manga::OpenMode::Metadata)) {
        return false;
      }
      cachePath = manga::cachePath(fullPath);
      title = boundedMetadata(mangaBook.title());
      author = boundedMetadata(mangaBook.author());
      thumbPath = manga::thumbnailTemplatePath(fullPath);
      if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(cachePath.c_str())) {
        LOG_ERR("BookActions", "Failed to create manga stats cache: %s", cachePath.c_str());
        return false;
      }
    }

    stats = BookReadingStats::load(cachePath);
    globalStats = GlobalReadingStats::load();
    if (!stats.persistenceWritable || !globalStats.persistenceWritable) {
      LOG_ERR("BookActions", "Cannot begin completion edit after a failed stats read");
      return false;
    }
    completed = !stats.isCompleted;
    stats.isCompleted = completed;
    if (completed && !stats.finishedDateManual) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now)) {
        stats.finishedDate = now.date;
      }
    }

    if (completed) {
      globalStats.completedBooks++;
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }

    edit.initialized = true;
    edit.persistence.changed();
  }
  completed = stats.isCompleted;
  if (!edit.persistence.persist(cachePath, stats, globalStats)) return false;
  if (edit.sideEffectsDone) return true;
  edit.sideEffectsDone = true;

  if (SETTINGS.removeReadBooksFromRecents) {
    if (completed) {
      RECENT_BOOKS.removeByPath(fullPath);
    } else {
      RECENT_BOOKS.addOrUpdateBook(fullPath, title, author, thumbPath);
    }
  }

  if (isEpub && completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    const std::string oldCachePath = cachePath;
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    BookMoveUtils::migrateMovedEpubState(fullPath, dstPath, oldCachePath, title, author,
                                         !SETTINGS.removeReadBooksFromRecents);
  }

  return true;
}

void startCompletionEdit(Activity& owner, GfxRenderer& renderer, MappedInputManager& input, const std::string& fullPath,
                         const std::string& displayName, ActivityResultHandler handler) {
  auto activity = makeUniqueNoThrow<BookCompletionActivity>(renderer, input, fullPath, displayName);
  if (!activity) {
    LOG_ERR("BookActions", "OOM for completion edit activity");
    return;
  }
  owner.startActivityForResult(std::move(activity), std::move(handler));
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}

}  // namespace BookActions
