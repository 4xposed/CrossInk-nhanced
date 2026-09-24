#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "BookDeletionSnapshot.h"
#include "HalStorage.h"
#include "test/UniqueTempDirectory.h"

namespace {
class BookDeletionSnapshotTest : public testing::Test {
 protected:
  void SetUp() override {
    root = uniqueTempDirectory("crossink-book-delete-snapshot");
    std::filesystem::create_directories(root);
    storage_test::reset();
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('x');
  }
  std::filesystem::path root;
};

TEST_F(BookDeletionSnapshotTest, CapturesNestedMangaAndSupportedBookFilesOnly) {
  touch(root / "UPPER.EPUB");
  touch(root / "deck.cdeck");
  touch(root / "legacy.xtch");
  touch(root / "series/volume/panels.idx");
  touch(root / "series/volume/page_0000.jpg");
  BookDeletionSnapshot snapshot;
  ASSERT_TRUE(snapshot.collect(root.string()));
  ASSERT_EQ(snapshot.count(), 4U);
  size_t manga = 0;
  for (size_t i = 0; i < snapshot.count(); ++i)
    if (snapshot.kind(i) == BookDeletionSnapshot::Kind::Manga) ++manga;
  EXPECT_EQ(manga, 1U);
  EXPECT_EQ(storage_test::handles, 0);
  EXPECT_EQ(storage_test::implicitCloses, 0);
}

TEST_F(BookDeletionSnapshotTest, MissingOpenNameAndCloseFailuresAbortWithoutHandles) {
  BookDeletionSnapshot snapshot;
  EXPECT_FALSE(snapshot.collect((root / "missing").string()));
  touch(root / "book.epub");
  storage_test::failName = (root / "book.epub").string();
  EXPECT_FALSE(snapshot.collect(root.string()));
  storage_test::reset();
  storage_test::failClose = (root / "book.epub").string();
  EXPECT_FALSE(snapshot.collect(root.string()));
  EXPECT_EQ(storage_test::handles, 0);
}

TEST_F(BookDeletionSnapshotTest, EntryAndDirectoryOverflowAbortWithoutHandles) {
  BookDeletionSnapshot snapshot;
  for (size_t i = 0; i <= BookDeletionSnapshot::MAX_ENTRIES; ++i) touch(root / (std::to_string(i) + ".epub"));
  EXPECT_FALSE(snapshot.collect(root.string()));
  EXPECT_EQ(snapshot.count(), 0U);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  for (size_t i = 0; i <= BookDeletionSnapshot::MAX_DIRECTORIES; ++i)
    std::filesystem::create_directories(root / ("dir" + std::to_string(i)));
  storage_test::reset();
  EXPECT_FALSE(snapshot.collect(root.string()));
  EXPECT_EQ(storage_test::handles, 0);
}

TEST_F(BookDeletionSnapshotTest, ArenaOverflowRejectsOversizedRootWithoutOpeningStorage) {
  BookDeletionSnapshot snapshot;
  EXPECT_FALSE(snapshot.collect(std::string(BookDeletionSnapshot::ARENA_BYTES, 'x')));
  EXPECT_EQ(snapshot.count(), 0U);
  EXPECT_EQ(storage_test::handles, 0);
}

TEST_F(BookDeletionSnapshotTest, ChildOpenFailureMustNotLookLikeEndOfDirectory) {
  touch(root / "book.epub");
  storage_test::failOpen = (root / "book.epub").string();
  BookDeletionSnapshot snapshot;
  EXPECT_FALSE(snapshot.collect(root.string()));
  EXPECT_EQ(snapshot.count(), 0U);
  EXPECT_EQ(storage_test::handles, 0);
}
TEST_F(BookDeletionSnapshotTest, DepthCeilingAllowsFilesAndEmptyDirectoriesButRejectsDeeperDirectory) {
  const auto deepest = root / "a/b/c/d/e/f/g/h";
  touch(deepest / "panels.idx");
  touch(deepest / "story.txt");
  std::filesystem::create_directories(root / "a/b/c/d/e/f/g/empty");
  BookDeletionSnapshot snapshot;
  ASSERT_TRUE(snapshot.collect(root.string(), SnapshotMode::DeleteMetadata, 8));
  EXPECT_EQ(snapshot.count(), 2U);
  EXPECT_EQ(storage_test::handles, 0);
  std::filesystem::create_directories(deepest / "too-deep");
  EXPECT_FALSE(snapshot.collect(root.string(), SnapshotMode::DeleteMetadata, 8));
  EXPECT_EQ(snapshot.count(), 0U);
  EXPECT_EQ(storage_test::handles, 0);
}
}  // namespace
