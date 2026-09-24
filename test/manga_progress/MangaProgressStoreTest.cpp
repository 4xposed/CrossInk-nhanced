#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

#include "HalStorage.h"
#include "MangaProgressStore.h"
#include "test/UniqueTempDirectory.h"

namespace {
class MangaProgressStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = uniqueTempDirectory("crossink-manga-progress-test");
    std::filesystem::create_directories(root_);
    manga_progress_test::reset(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path statePath() const { return root_ / ".crosspoint/manga-state/2705043005.bin"; }
  std::filesystem::path root_;
};

TEST_F(MangaProgressStoreTest, MissingFileReturnsFalseAndDefaults) {
  manga::Progress progress{42, 8, true, false};
  EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").load(progress));
  EXPECT_EQ(progress.page, 0U);
  EXPECT_EQ(progress.panel, -1);
  EXPECT_FALSE(progress.panelsOnly);
  EXPECT_TRUE(progress.rotatePanels);
}

TEST_F(MangaProgressStoreTest, ExposesStableStatePath) {
  const manga::MangaProgressStore store("/Books/example.cbz");
  EXPECT_EQ(store.statePath(), "/.crosspoint/manga-state/2705043005.bin");
  EXPECT_EQ(manga::MangaProgressStore::statePath("/Books/example.cbz"), store.statePath());
}

TEST_F(MangaProgressStoreTest, RemoveDeletesPrimaryBackupAndTemporaryState) {
  manga::MangaProgressStore store("/Books/example.cbz");
  ASSERT_TRUE(store.save({12, 1, false, true}));
  std::ofstream(statePath().string() + ".bak", std::ios::binary).put('B');
  std::ofstream(statePath().string() + ".tmp", std::ios::binary).put('T');

  EXPECT_TRUE(store.remove());
  EXPECT_FALSE(std::filesystem::exists(statePath()));
  EXPECT_FALSE(std::filesystem::exists(statePath().string() + ".bak"));
  EXPECT_FALSE(std::filesystem::exists(statePath().string() + ".tmp"));
  EXPECT_TRUE(store.remove());
}

TEST_F(MangaProgressStoreTest, SavesStablePathAndLoadsAfterReopen) {
  const manga::Progress saved{321, 17, true, false};
  EXPECT_TRUE(manga::MangaProgressStore("/Books/example.cbz").save(saved));
  EXPECT_TRUE(std::filesystem::exists(statePath()));
  EXPECT_EQ(std::filesystem::file_size(statePath()), 12U);

  manga::Progress loaded;
  EXPECT_TRUE(manga::MangaProgressStore("/Books/example.cbz").load(loaded));
  EXPECT_EQ(loaded.page, saved.page);
  EXPECT_EQ(loaded.panel, saved.panel);
  EXPECT_EQ(loaded.panelsOnly, saved.panelsOnly);
  EXPECT_EQ(loaded.rotatePanels, saved.rotatePanels);
  EXPECT_EQ(manga_progress_test::openHandles(), 0);
  EXPECT_EQ(manga_progress_test::implicitCloses(), 0);
}

TEST_F(MangaProgressStoreTest, WritesVersionedLittleEndianBytes) {
  ASSERT_TRUE(manga::MangaProgressStore("/Books/example.cbz").save({0x1234, 2, true, false}));
  std::array<uint8_t, 12> bytes{};
  std::ifstream file(statePath(), std::ios::binary);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  const std::array<uint8_t, 12> expected{'M', 'G', 'P', 'R', 1, 1, 2, 0, 0x34, 0x12, 0, 0};
  EXPECT_EQ(bytes, expected);
}

TEST_F(MangaProgressStoreTest, RejectsCorruptOrInvalidRecordsAndDefaultsOutput) {
  const std::array<std::array<uint8_t, 12>, 6> invalid{{
      {{'X', 'G', 'P', 'R', 1, 0, 0xff, 0xff, 0, 0, 0, 0}},
      {{'M', 'G', 'P', 'R', 2, 0, 0xff, 0xff, 0, 0, 0, 0}},
      {{'M', 'G', 'P', 'R', 1, 4, 0xff, 0xff, 0, 0, 0, 0}},
      {{'M', 'G', 'P', 'R', 1, 0, 0xfe, 0xff, 0, 0, 0, 0}},
      {{'M', 'G', 'P', 'R', 1, 0, 0xff, 0x00, 0, 0, 0, 0}},
      {{'M', 'G', 'P', 'R', 1, 0, 0xff, 0xff, 0x10, 0x27, 0, 0}},
  }};
  std::filesystem::create_directories(statePath().parent_path());
  for (const auto& bytes : invalid) {
    std::ofstream file(statePath(), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    manga::Progress output{9, 3, true, false};
    EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").load(output));
    EXPECT_EQ(output.page, 0U);
    EXPECT_EQ(output.panel, -1);
    EXPECT_FALSE(output.panelsOnly);
    EXPECT_TRUE(output.rotatePanels);
  }
}

TEST_F(MangaProgressStoreTest, RejectsWrongLengthAndSaveRange) {
  std::filesystem::create_directories(statePath().parent_path());
  std::ofstream(statePath(), std::ios::binary).put('M');
  manga::Progress output{9, 3, true, false};
  EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").load(output));
  EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").save({10000, -1, false, true}));
  EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").save({1, -2, false, true}));
  EXPECT_FALSE(manga::MangaProgressStore("/Books/example.cbz").save({1, 255, false, true}));
}

TEST_F(MangaProgressStoreTest, FailedReplacementPreservesOldGoodState) {
  manga::MangaProgressStore store("/Books/example.cbz");
  ASSERT_TRUE(store.save({12, 1, false, true}));
  manga_progress_test::failRenameOnCall(2);
  EXPECT_FALSE(store.save({99, 7, true, false}));

  manga::Progress loaded;
  EXPECT_TRUE(store.load(loaded));
  EXPECT_EQ(loaded.page, 12U);
  EXPECT_EQ(loaded.panel, 1);
  EXPECT_EQ(manga_progress_test::openHandles(), 0);
  EXPECT_EQ(manga_progress_test::implicitCloses(), 0);
}

TEST_F(MangaProgressStoreTest, WriteSyncAndCloseFailuresLeaveOldStateReadable) {
  for (const auto failure :
       {manga_progress_test::Failure::Write, manga_progress_test::Failure::Sync, manga_progress_test::Failure::Close}) {
    manga::MangaProgressStore store("/Books/example.cbz");
    ASSERT_TRUE(store.save({12, 1, false, true}));
    manga_progress_test::failNext(failure);
    EXPECT_FALSE(store.save({99, 7, true, false}));
    manga::Progress loaded;
    EXPECT_TRUE(store.load(loaded));
    EXPECT_EQ(loaded.page, 12U);
    EXPECT_EQ(manga_progress_test::openHandles(), 0);
    std::filesystem::remove_all(root_ / ".crosspoint");
  }
}
}  // namespace
