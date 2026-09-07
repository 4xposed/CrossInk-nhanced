#pragma once
#include "ReadingStatsSave.h"
namespace manga {
// One immutable transaction plus an unbanked activation tail. Inline ownership
// avoids large caller stack copies and heap failure during the cache-delete gate.
class StatsCommit {
 public:
  void changed() { dirty_ = true; }
  bool pending() const { return active_; }
  bool failed() const { return failed_; }
  bool flush(const std::string& path, BookReadingStats& book, GlobalReadingStats& global, uint32_t activationMs,
             const ReadingStatsDateTime& localStart, std::string_view language);

 private:
  BookReadingStats frozenBook_;
  GlobalReadingStats frozenGlobal_;
  ReadingLanguageSpan span_;
  ReadingStatsSaveResult saved_;
  uint32_t bankedSeconds_ = 0;
  bool active_ = false, dirty_ = false, countedSession_ = false, failed_ = false;
  bool publish(const std::string& path);
};
static_assert(sizeof(StatsCommit) <= 512, "Manga stats retry state must remain bounded");
}  // namespace manga
