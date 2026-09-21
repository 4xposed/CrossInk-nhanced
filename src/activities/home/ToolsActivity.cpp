#include "ToolsActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <Memory.h>
#include <Xtc.h>

#include "CrossPointState.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "SavedItemsHomeActivity.h"
#include "activities/reader/BookStatsActivity.h"
const char* ToolsActivity::itemLabel(int index) const {
  static constexpr StrId labels[] = {StrId::STR_BROWSE_FILES, StrId::STR_MENU_RECENT_BOOKS, StrId::STR_READING_STATS,
                                     StrId::STR_BOOKMARKS_AND_CLIPPINGS};
  return I18N.get(labels[index]);
}
void ToolsActivity::activate(int index) {
  if (index == 0) {
    activityManager.goToFileBrowser();
    return;
  }
  if (index == 1) {
    activityManager.goToRecentBooks();
    return;
  }
  std::unique_ptr<Activity> next;
  if (index == 3)
    next = makeUniqueNoThrow<SavedItemsHomeActivity>(renderer, mappedInput);
  else if (index == 2) {
    const std::string path = APP_STATE.openEpubPath;
    const std::string cache = FsHelpers::hasEpubExtension(path)  ? Epub::cachePathForFilePath(path, "/.crosspoint")
                              : FsHelpers::hasXtcExtension(path) ? Xtc(path, "/.crosspoint").getCachePath()
                              : (!path.empty() && manga::MangaBook::isMangaFolder(path.c_str()))
                                  ? manga::cachePath(path)
                                  : "";
    // Statistics aggregate contains language arrays; keep it off the task stack.
    auto global = makeUniqueNoThrow<GlobalReadingStats>();
    auto aggregate = makeUniqueNoThrow<GlobalReadingStats>();
    auto stats = makeUniqueNoThrow<BookReadingStats>();
    if (!global || !aggregate || !stats) {
      LOG_ERR("Tools", "Cannot allocate statistics");
      return;
    }
    *global = GlobalReadingStats::load();
    *aggregate = GlobalReadingStats::loadAggregated(*global);
    if (!cache.empty()) *stats = BookReadingStats::load(cache);
    RecentBook book;
    book.path = path;
    book.title = path.substr(path.find_last_of('/') + 1);
    next = makeUniqueNoThrow<BookStatsActivity>(
        renderer, mappedInput, book.title.empty() ? tr(STR_READING_STATS) : book.title, cache, *stats,
        path.empty() ? -1.0f : RecentBookProgress::loadPercent(book), false, 0, *global, *aggregate, false);
  }
  if (next)
    startActivityForResult(std::move(next), [this](const ActivityResult&) { requestUpdate(); });
  else
    LOG_ERR("Tools", "Cannot allocate destination");
}
