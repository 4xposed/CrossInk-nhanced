#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "MangaStatsCommit.h"

namespace {
struct Day {
  uint32_t id;
  uint32_t seconds;
};
std::vector<Day> days(const char* path) {
  std::vector<Day> result;
  assert(visitLocalReadingLanguageDays(
      path,
      [](void* ctx, uint32_t id, const ReadingLanguageTotals& totals) {
        uint32_t seconds = 0;
        for (const auto& entry : totals.entries) {
          if (entry.seconds) assert(std::string(entry.tag) == "ja");
          seconds += entry.seconds;
        }
        static_cast<std::vector<Day>*>(ctx)->push_back({id, seconds});
        return true;
      },
      &result));
  return result;
}
void assertDays(const char* path, uint32_t total) {
  const auto rows = days(path);
  assert(rows.size() == 2 && rows[1].id == rows[0].id + 1);
  assert(rows[0].seconds == 10 && rows[1].seconds == total - 10);
}
void matrix(const std::filesystem::path& dir, int scenario) {
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / ".crosspoint/book");
  storage_test::root = dir.string();
  storage_test::failWritePath.clear();
  storage_test::failWrite = false;
  BookReadingStats book;
  GlobalReadingStats global;
  manga::StatsCommit commit;
  ReadingStatsDateTime start{{2026, 9, 7}, 23, 59, 50};
  // Reverse target failure, both targets failing, and T1 completion followed by
  // partial T2 failure. Inspect actual daily/language rows, not just summaries.
  if (scenario == 1)
    storage_test::failWrite = true;
  else
    storage_test::failWritePath = "/book/";
  assert(!commit.flush("/.crosspoint/book", book, global, 65750, start, "ja"));
  if (scenario != 1) {
    assert(GlobalReadingStats::load().totalReadingSeconds == 65);
    assertDays("/.crosspoint/global_stats.bin", 65);
  }
  book.totalPagesTurned = 2;
  book.isCompleted = true;
  global.totalPagesTurned = 2;
  global.completedBooks = 1;
  commit.changed();
  if (scenario == 1) {
    storage_test::failWrite = false;
    storage_test::failWritePath = "global_stats";
    assert(!commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
    assert(BookReadingStats::load("/.crosspoint/book").totalReadingSeconds == 65);
    assert(!BookReadingStats::load("/.crosspoint/book").isCompleted);
    assertDays("/.crosspoint/book/stats_v6.bin", 65);
    assert(!commit.flush("/.crosspoint/book", book, global, 77900, start, "ja"));
    assertDays("/.crosspoint/book/stats_v6.bin", 65);
  } else if (scenario == 2) {
    // T1's successful global span is not replayed. T1 book retry succeeds, then
    // the new 12-second T2 succeeds only for the book and freezes independently.
    storage_test::failWritePath = "global_stats";
    assert(!commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
    assertDays("/.crosspoint/book/stats_v6.bin", 77);
    assertDays("/.crosspoint/global_stats.bin", 65);
    book.totalPagesTurned = 3;
    commit.changed();
    assert(!commit.flush("/.crosspoint/book", book, global, 77900, start, "ja"));
    assertDays("/.crosspoint/book/stats_v6.bin", 77);
    assert(BookReadingStats::load("/.crosspoint/book").totalPagesTurned == 2);
  } else {
    assert(!commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
    assertDays("/.crosspoint/global_stats.bin", 65);
  }
  storage_test::failWritePath.clear();
  assert(commit.flush("/.crosspoint/book", book, global, 77900, start, "ja"));
  assertDays("/.crosspoint/book/stats_v6.bin", 77);
  assertDays("/.crosspoint/global_stats.bin", 77);
  // 900 ms retained from the prior activation total plus 200 ms becomes one
  // later saved second. Per-flush flooring would lose this carry.
  assert(commit.flush("/.crosspoint/book", book, global, 78100, start, "ja"));
  assertDays("/.crosspoint/book/stats_v6.bin", 78);
  assertDays("/.crosspoint/global_stats.bin", 78);
  assert(book.totalReadingSeconds == 78 && global.totalReadingSeconds == 78);
  assert(book.sessionCount == 1 && global.totalSessions == 1);
  assert(commit.flush("/.crosspoint/book", book, global, 78100, start, "ja"));
  assertDays("/.crosspoint/book/stats_v6.bin", 78);
  assertDays("/.crosspoint/global_stats.bin", 78);
  assert(storage_test::openFiles == 0);
}
}  // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "crossink_manga_menu_stats";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / ".crosspoint/book");
  storage_test::root = dir.string();
  BookReadingStats book;
  GlobalReadingStats global;
  manga::StatsCommit commit;
  ReadingStatsDateTime start{{2026, 9, 7}, 10, 0, 0};
  storage_test::failWritePath = "global_stats";
  assert(!commit.flush("/.crosspoint/book", book, global, 65000, start, "ja"));
  assert(commit.failed() && commit.pending());
  assert(BookReadingStats::load("/.crosspoint/book").totalReadingSeconds == 65);
  int writes = storage_test::bookWriteOpens;
  book.totalPagesTurned = 1;
  global.totalPagesTurned = 1;
  book.isCompleted = true;
  global.completedBooks = 1;
  commit.changed();
  assert(!commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
  assert(storage_test::bookWriteOpens == writes);
  assert(book.totalReadingSeconds == 65 && book.sessionCount == 1);
  assert(!BookReadingStats::load("/.crosspoint/book").isCompleted);
  storage_test::failWritePath.clear();
  assert(commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
  assert(BookReadingStats::load("/.crosspoint/book").totalReadingSeconds == 77);
  assert(GlobalReadingStats::load().totalReadingSeconds == 77);
  assert(book.sessionCount == 1 && global.totalSessions == 1 && !commit.pending());
  writes = storage_test::bookWriteOpens;
  assert(commit.flush("/.crosspoint/book", book, global, 77500, start, "ja"));
  assert(storage_test::bookWriteOpens == writes);
  assert(commit.flush("/.crosspoint/book", book, global, 120000, start, "ja"));
  assert(book.totalReadingSeconds == 120 && book.startDate.isValid() && book.sessionCount == 1);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / ".crosspoint/book");
  book = {};
  global = {};
  commit = {};
  assert(commit.flush("/.crosspoint/book", book, global, 20000, start, "en"));
  assert(commit.flush("/.crosspoint/book", book, global, 25000, start, "en"));
  assert(book.totalReadingSeconds == 25 && global.totalReadingSeconds == 25);
  assert(commit.flush("/.crosspoint/book", book, global, 40000, start, "en"));
  assert(book.sessionCount == 0);
  assert(commit.flush("/.crosspoint/book", book, global, 65000, start, "en"));
  assert(book.sessionCount == 1 && global.totalSessions == 1);
  for (int scenario = 0; scenario < 3; ++scenario) matrix(dir, scenario);
  printf("Stats retry/tail/threshold contracts passed; state=%zu bytes\n", sizeof(commit));
  std::filesystem::remove_all(dir);
}
