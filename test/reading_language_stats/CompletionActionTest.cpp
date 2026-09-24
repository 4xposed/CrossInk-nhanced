#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>

#include "ReadingStatsSave.h"
#include "activities/home/BookCompletionEdit.h"
#include "test/UniqueTempDirectory.h"

// Metadata and UI are doubles; extracted production completion code below
// publishes through the real per-book/global stores and fault-injected HAL.
struct Epub {
  Epub(const std::string&, const char*) {}
  void setupCacheDir() {}
  std::string getCachePath() const { return "/.crosspoint/book"; }
  std::string getTitle() const { return "Book"; }
  std::string getAuthor() const { return "Author"; }
  std::string getThumbBmpPath() const { return "thumb"; }
};
struct Xtc : Epub {
  using Epub::Epub;
  bool load() { return true; }
};
namespace FsHelpers {
bool hasEpubExtension(const std::string&) { return true; }
bool hasXtcExtension(const std::string&) { return false; }
}  // namespace FsHelpers
namespace manga {
enum class OpenMode { Metadata };
struct MangaBook {
  static bool isMangaFolder(const char*) { return false; }
  bool open(const char*, OpenMode) { return false; }
  std::string title() const { return {}; }
  std::string author() const { return {}; }
};
std::string cachePath(const std::string&) { return "/.crosspoint/book"; }
std::string thumbnailTemplatePath(const std::string&) { return {}; }
}  // namespace manga
std::string boundedMetadata(const std::string& value) { return value; }
struct Settings {
  bool removeReadBooksFromRecents = true, moveFinishedToReadFolder = false;
} SETTINGS;
struct Recents {
  int updates = 0;
  void removeByPath(const std::string&) { ++updates; }
  void addOrUpdateBook(const std::string&, const std::string&, const std::string&, const std::string&) { ++updates; }
} RECENT_BOOKS;
struct AppState {
  char pendingAlertTitle[128]{}, pendingAlertBody[256]{};
  std::atomic<bool> pendingAlertGoHomeOnBack{false}, hasPendingAlert{false};
} APP_STATE;
namespace BookMoveUtils {
std::string buildReadFolderDestination(const std::string&) { return {}; }
void migrateMovedEpubState(const std::string&, const std::string&, const std::string&, const std::string&,
                           const std::string&, bool) {}
}  // namespace BookMoveUtils
constexpr int STR_MOVE_TO_READ_FAILED_TITLE = 1, STR_MOVE_TO_READ_FAILED_BODY = 2;
const char* tr(int) { return "%s"; }
namespace BookActions {
#include "CompletionAction.inc"
}

class CompletionActionTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    storage_test::root = uniqueTempDirectory("crossink-completion-action-fixture").string();
    std::filesystem::create_directories(storage_test::root + "/.crosspoint/book");
    RECENT_BOOKS.updates = 0;
    halClock.available = false;
    BookReadingStats book;
    book.finishedDate = {2026, 9, 6};
    book.finishedDateManual = true;
    ASSERT_TRUE(book.save("/.crosspoint/book"));
    GlobalReadingStats global;
    global.completedBooks = 7;
    ASSERT_TRUE(global.save());
  }
  void TearDown() override {
    halClock.available = false;
    storage_test::failWritePath.clear();
    EXPECT_EQ(storage_test::openFiles, 0);
    std::filesystem::remove_all(storage_test::root);
  }
};
TEST_P(CompletionActionTest, PartialPublishRetriesSameDesiredValueAndCountExactlyOnce) {
  storage_test::failWritePath = GetParam() ? "/.crosspoint/book/stats_v6.bin.tmp" : "/.crosspoint/global_stats.bin.tmp";
  bool completed = false;
  BookActions::CompletionEdit edit;
  ASSERT_FALSE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_EQ(RECENT_BOOKS.updates, 0);
  ASSERT_FALSE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_EQ(RECENT_BOOKS.updates, 0);
  EXPECT_TRUE(BookReadingStats::load("/.crosspoint/book").isCompleted || GetParam());
  storage_test::failWritePath.clear();
  ASSERT_TRUE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_TRUE(completed);
  const auto book = BookReadingStats::load("/.crosspoint/book");
  EXPECT_TRUE(book.isCompleted);
  EXPECT_EQ(book.finishedDate.day, 6);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 8u);
  EXPECT_EQ(RECENT_BOOKS.updates, 1);
  const int writeCalls = storage_test::writeCalls;
  EXPECT_TRUE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_EQ(storage_test::writeCalls, writeCalls);
  EXPECT_EQ(RECENT_BOOKS.updates, 1);
}
TEST_P(CompletionActionTest, AutomaticFinishedDateAndDesiredValueSurviveLaterExplicitRetry) {
  auto initial = BookReadingStats::load("/.crosspoint/book");
  initial.finishedDateManual = false;
  ASSERT_TRUE(initial.save("/.crosspoint/book"));
  halClock.available = true;
  halClock.day = 7;
  storage_test::failWritePath = GetParam() ? "/.crosspoint/book/stats_v6.bin.tmp" : "/.crosspoint/global_stats.bin.tmp";
  BookActions::CompletionEdit edit;
  bool completed = false;
  EXPECT_FALSE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_TRUE(edit.persistence.dirty);
  EXPECT_EQ(edit.book.finishedDate.day, 7);
  halClock.day = 8;
  EXPECT_FALSE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_TRUE(completed);
  EXPECT_EQ(edit.book.finishedDate.day, 7);
  storage_test::failWritePath.clear();
  EXPECT_TRUE(BookActions::toggleBookCompleted("/book.epub", "Book", completed, edit));
  EXPECT_FALSE(edit.persistence.dirty);
  EXPECT_EQ(BookReadingStats::load("/.crosspoint/book").finishedDate.day, 7);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 8u);
}
INSTANTIATE_TEST_SUITE_P(BookOrGlobalFailure, CompletionActionTest, testing::Bool());
