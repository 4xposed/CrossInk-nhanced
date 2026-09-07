#include "ReadingStatsSave.h"

#include <Logging.h>
ReadingStatsSaveResult saveReadingStatsWithRetry(const std::string& cachePath, const BookReadingStats& book,
                                                 const GlobalReadingStats& global, const ReadingLanguageSpan* span,
                                                 ReadingStatsSaveResult saved) {
  const bool tryBook = !saved.book;
  const bool tryGlobal = !saved.global;
  if (tryBook) saved.book = cachePath.empty() || book.save(cachePath, span);
  if (tryGlobal) saved.global = global.save(span);
  if (tryBook && !saved.book) {
    LOG_ERR("STATS", "Book save failed; retrying once");
    saved.book = book.save(cachePath, span);
  }
  if (tryGlobal && !saved.global) {
    LOG_ERR("GSTATS", "Global save failed; retrying once");
    saved.global = global.save(span);
  }
  if (!saved.book) LOG_ERR("STATS", "Book save remains failed; span not persisted");
  if (!saved.global) LOG_ERR("GSTATS", "Global save remains failed; span not persisted");
  return saved;
}
bool ReadingStatsEditState::persist(const std::string& path, const BookReadingStats& book,
                                    const GlobalReadingStats& global) {
  if (!dirty) return true;
  saved = saveReadingStatsWithRetry(path, book, global, nullptr, saved);
  failed = !saved.complete();
  if (!failed) dirty = false;
  return !dirty;
}
