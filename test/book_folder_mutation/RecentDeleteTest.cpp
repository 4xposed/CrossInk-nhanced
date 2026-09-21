#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>

#include "test/UniqueTempDirectory.h"
#include "BookDeletionSnapshot.h"
#include "BookFolderMutation.h"
#include "BookMutationJournal.h"
#include "BookMutationStorage.h"

// Platform/UI doubles. The selected action callbacks below are extracted
// verbatim and linked to the real transaction, journal and metadata code.
struct RecentBook {
  std::string path, title;
};
struct ActivityResult {
  bool isCancelled = false;
};
struct ConfirmationActivity {
  ConfirmationActivity(int, int, const std::string&, const std::string&) {}
};
struct RecentStore {
  void removeByPath(const std::string&) {}
} RECENT_BOOKS;
namespace manga {
struct MangaBook {
  static bool isMangaFolder(const char* path) { return Storage.exists((std::string(path) + "/panels.idx").c_str()); }
};
}  // namespace manga
namespace BookActions {
bool clearMangaMetadata(const std::string&) { return true; }
void clearFileMetadata(const std::string&) {}
void drawToast(int, const char*) {}
}  // namespace BookActions
constexpr int STR_DELETE = 1, STR_METADATA_RECOVERY_PENDING = 2, STR_ERROR_GENERAL_FAILURE = 3;
const char* tr(int) { return "message"; }
struct RecentActionOwner {
  int renderer = 0, mappedInput = 0, reloads = 0;
  void reloadAfterBookAction() { ++reloads; }
  template <class F>
  void startActivityForResult(std::unique_ptr<ConfirmationActivity>, F handler) {
    handler(ActivityResult{});
  }
};
struct RecentBooksActivity : RecentActionOwner {
  void promptDeleteBook(const RecentBook&);
};
struct RecentBooksGridActivity : RecentActionOwner {
  void promptDeleteBook(const RecentBook&);
};
#include "RecentDeleteActions.inc"

class RecentDeleteTest : public testing::TestWithParam<bool> {
 protected:
  std::filesystem::path root;
  void SetUp() override {
    root = uniqueTempDirectory("crossink-recent-delete-native");
    std::filesystem::create_directories(root);
    mutation_test::reset(root.string());
    BookFolderMutation::recoverPending();
    put("/Book/panels.idx", "marker");
    put("/Book/Child/panels.idx", "marker");
    put("/Book/Survivor/panels.idx", "marker");
    put(cache("/Book/Child") + "/dictionary.bin", "child");
    put(cache("/Book/Survivor") + "/dictionary.bin", "survivor");
    put("/.crosspoint/recent.json", R"({"books":[{"path":"/Book"},{"path":"/Book/Child"},{"path":"/Book/Survivor"}]})");
    put("/.crosspoint/state.json", R"({"openEpubPath":"/Book","lastSleepFromReader":true,"future":7})");
  }
  void TearDown() override {
    EXPECT_EQ(mutation_test::handles, 0);
    mutation_test::offline = false;
    mutation_test::failPath.clear();
    mutation_test::failReload = false;
    std::filesystem::remove_all(root);
  }
  void put(const std::string& path, const std::string& text) {
    const auto file = root / path.substr(1);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
  }
  std::string get(const std::string& path) {
    std::ifstream file(root / path.substr(1));
    return {std::istreambuf_iterator<char>(file), {}};
  }
  std::string cache(const char* path) {
    char out[64];
    bookmutation::mangaCachePath(path, out, sizeof(out));
    return out;
  }
  int remove() {
    RecentBook book{"/Book", "Book"};
    if (GetParam()) {
      RecentBooksGridActivity owner;
      owner.promptDeleteBook(book);
      return owner.reloads;
    }
    RecentBooksActivity owner;
    owner.promptDeleteBook(book);
    return owner.reloads;
  }
};
TEST_P(RecentDeleteTest, SuccessClearsResumeAndNestedMetadataBeforeReload) {
  EXPECT_EQ(remove(), 1);
  EXPECT_EQ(get("/.crosspoint/recent.json"), R"({"books":[]})");
  EXPECT_EQ(get("/.crosspoint/state.json"), R"({"openEpubPath":"","lastSleepFromReader":true,"future":7})");
  EXPECT_FALSE(Storage.exists((cache("/Book/Child") + "/dictionary.bin").c_str()));
}
TEST_P(RecentDeleteTest, PartialDeletionCleansOnlyAbsentPaths) {
  mutation_test::partialDeleteChild = "/Book/Child";
  EXPECT_EQ(remove(), 0);
  EXPECT_FALSE(Storage.exists("/Book/Child"));
  EXPECT_TRUE(Storage.exists("/Book/Survivor/panels.idx"));
  EXPECT_EQ(get(cache("/Book/Survivor") + "/dictionary.bin"), "survivor");
  EXPECT_EQ(get("/.crosspoint/recent.json"), R"({"books":[{"path":"/Book"},{"path":"/Book/Survivor"}]})");
}
TEST_P(RecentDeleteTest, MetadataFailureRetainsJournalAndRecoveryFinishesCleanup) {
  const auto damagedCache = cache("/Book/Child") + "/nested/pixels";
  put(damagedCache, "cached");
  mutation_test::failPath = damagedCache;
  EXPECT_EQ(remove(), 0);
  EXPECT_TRUE(BookFolderMutation::hasPending());
  EXPECT_FALSE(Storage.exists("/Book"));
  mutation_test::failPath.clear();
  EXPECT_EQ(BookFolderMutation::recoverPending(), BookFolderMutation::Result::Complete);
  EXPECT_EQ(get("/.crosspoint/recent.json"), R"({"books":[]})");
  EXPECT_EQ(get("/.crosspoint/state.json"), R"({"openEpubPath":"","lastSleepFromReader":true,"future":7})");
}
TEST_P(RecentDeleteTest, RecreatedPathRetainsMetadataOnRecovery) {
  mutation_test::cutPhase = int(bookmutation::Phase::ContentSucceeded);
  EXPECT_EQ(remove(), 0);
  EXPECT_TRUE(BookFolderMutation::hasPending());
  mutation_test::offline = false;
  mutation_test::cutPhase = 0;
  put("/Book/panels.idx", "recreated");
  EXPECT_EQ(BookFolderMutation::recoverPending(), BookFolderMutation::Result::MutationFailed);
  EXPECT_NE(get("/.crosspoint/recent.json").find("/Book"), std::string::npos);
  EXPECT_EQ(get("/Book/panels.idx"), "recreated");
}
INSTANTIATE_TEST_SUITE_P(ListAndGrid, RecentDeleteTest, testing::Bool());
