#include "MangaStatsCommit.h"

#include <algorithm>
namespace manga {
bool StatsCommit::publish(const std::string& path) {
  saved_ = saveReadingStatsWithRetry(path, frozenBook_, frozenGlobal_, &span_, saved_);
  failed_ = !saved_.complete();
  if (!failed_) active_ = false;
  return !failed_;
}
bool StatsCommit::flush(const std::string& path, BookReadingStats& book, GlobalReadingStats& global,
                        const uint32_t activationMs, const ReadingStatsDateTime& localStart,
                        const std::string_view language) {
  // A failed older target always receives its frozen snapshot/span first. New
  // page mutations and accepted time never replace it or replay a successful file.
  if (active_ && !publish(path)) return false;
  const uint32_t seconds = activationMs / 1000;
  const uint32_t delta = seconds >= 10 && seconds > bankedSeconds_ ? seconds - bankedSeconds_ : 0;
  span_ = {};
  span_.localStart = localStart;
  addSecondsToReadingStatsDateTime(span_.localStart, bankedSeconds_);
  normalizeReadingLanguage(language, span_.normalizedTag);
  span_.seconds = delta;
  if (delta) {
    book.totalReadingSeconds = saturateReadingLanguageSeconds(book.totalReadingSeconds, delta);
    global.totalReadingSeconds = saturateReadingLanguageSeconds(global.totalReadingSeconds, delta);
    addReadingLanguageSeconds(book.languageTotals, span_.normalizedTag, delta);
    addReadingLanguageSeconds(global.languageTotals, span_.normalizedTag, delta);
    if (span_.localStart.isValid()) {
      book.recordReadingSpan(span_.localStart, delta);
      global.recordReadingSpan(span_.localStart, delta);
    }
    bankedSeconds_ = seconds;
    dirty_ = true;
  }
  if (seconds >= 60 && !countedSession_) {
    if (book.sessionCount < UINT16_MAX) ++book.sessionCount;
    if (global.totalSessions < UINT32_MAX) ++global.totalSessions;
    countedSession_ = true;
    dirty_ = true;
  }
  if (seconds >= 120 && !book.startDateManual && !book.startDate.isValid() && localStart.isValid()) {
    book.startDate = localStart.date;
    dirty_ = true;
  }
  if (!dirty_) return true;
  frozenBook_ = book;
  frozenGlobal_ = global;
  saved_ = {};
  active_ = true;
  dirty_ = false;
  return publish(path);
}
}  // namespace manga
