#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "BookFolderMutation.h"
#include "BookMutationJournal.h"
#include "BookMutationStorage.h"
#include "BookmarkStore.h"
#include "test/UniqueTempDirectory.h"
using namespace BookFolderMutation;
namespace {
class MutationTest : public testing::Test {
 protected:
  std::filesystem::path root;
  void SetUp() override {
    root = uniqueTempDirectory("crossink-mutation-native");
    std::filesystem::create_directories(root);
    mutation_test::reset(root.string());
    recoverPending();
  }
  void TearDown() override {
    mutation_test::offline = false;
    mutation_test::failAt = 0;
    mutation_test::failPath.clear();
    std::filesystem::remove_all(root);
  }
  void put(const std::string& path, const std::string& text) {
    auto file = root / path.substr(1);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
  }
  std::string get(const std::string& path) {
    std::ifstream f(root / path.substr(1), std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
  }
  std::string cache(const char* book) {
    char path[64];
    bookmutation::mangaCachePath(book, path, sizeof(path));
    return path;
  }
  void fixture() {
    put("/Series/Book/panels.idx", "fixture");
    put("/Series/Book/page.jpg", "pixels");
    put(cache("/Series/Book") + "/stats_v6.bin", std::string(26429, 's'));
    put(cache("/Series/Book") + "/dictionary.bin", "route");
    put(cache("/Series/Book") + "/dictionary_history.txt", "history");
    put("/.crosspoint/recent.json", "{\"unknown\":[8],\"books\":[{\"path\":\"/Series/Book\",\"title\":\"Book\"}]}");
    put("/.crosspoint/state.json", "{\"openEpubPath\":\"/Series/Book\",\"future\":73}");
  }
};
TEST_F(MutationTest, MovesAncestorWithWholeStatsDictionaryAndSharedReferences) {
  fixture();
  ASSERT_EQ(move("/Series", "/Moved"), Result::Complete);
  EXPECT_EQ(get(cache("/Moved/Book") + "/stats_v6.bin"), std::string(26429, 's'));
  EXPECT_EQ(get(cache("/Moved/Book") + "/dictionary.bin"), "route");
  EXPECT_EQ(get(cache("/Moved/Book") + "/dictionary_history.txt"), "history");
  EXPECT_NE(get("/.crosspoint/recent.json").find("/Moved/Book"), std::string::npos);
  EXPECT_NE(get("/.crosspoint/state.json").find("\"future\":73"), std::string::npos);
  EXPECT_FALSE(hasPending());
}
TEST_F(MutationTest, PhysicalDestinationCollisionChangesNothing) {
  fixture();
  put("/Moved/keep", "unrelated");
  auto before = get("/.crosspoint/recent.json");
  EXPECT_EQ(move("/Series", "/Moved"), Result::Collision);
  EXPECT_EQ(get("/Moved/keep"), "unrelated");
  EXPECT_EQ(get("/.crosspoint/recent.json"), before);
}
TEST_F(MutationTest, RejectsSourceAndDestinationAncestorCaseAliases) {
  fixture();
  EXPECT_EQ(move("/series", "/Moved"), Result::StorageError);
  put("/Target/keep", "x");
  EXPECT_EQ(move("/Series", "/target/Moved"), Result::StorageError);
  EXPECT_TRUE(std::filesystem::exists(root / "Series/Book/panels.idx"));
}
TEST_F(MutationTest, RecursiveDeleteCleansOnlyAfterBackingFolderDisappears) {
  fixture();
  ASSERT_EQ(BookFolderMutation::remove("/Series"), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / "Series"));
  EXPECT_EQ(get("/.crosspoint/recent.json"), "{\"unknown\":[8],\"books\":[]}");
  EXPECT_EQ(get("/.crosspoint/state.json"), "{\"openEpubPath\":\"\",\"future\":73}");
  EXPECT_FALSE(std::filesystem::exists(root / (cache("/Series/Book") + "/dictionary.bin").substr(1)));
}
TEST_F(MutationTest, ConfirmedDeleteRemovesEntireCacheBeforeMetadataPhaseAndReplays) {
  fixture();
  const auto owned = cache("/Series/Book");
  for (const char* name : {"pixels.bin", "thumb_v3.bin", "ocr.tmp", "cover.tmp", "unknown", "nested/deeper/pixels"})
    put(owned + "/" + name, "disposable");
  put(cache("/Unrelated") + "/keep", "retain");
  mutation_test::cutPhase = int(bookmutation::Phase::DeleteMetadataDone);
  ASSERT_EQ(BookFolderMutation::remove("/Series"), Result::RecoveryPending);
  EXPECT_FALSE(std::filesystem::exists(root / owned.substr(1)));
  EXPECT_FALSE(std::filesystem::exists(root / "Series"));
  EXPECT_EQ(mutation_test::handles, 0);
  mutation_test::offline = false;
  mutation_test::cutPhase = 0;
  EXPECT_EQ(recoverPending(), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / owned.substr(1)));
  EXPECT_EQ(get(cache("/Unrelated") + "/keep"), "retain");
  EXPECT_FALSE(hasPending());
}
TEST_F(MutationTest, CacheScanFailureKeepsDeletePendingUntilWholeCacheCanBeRemoved) {
  fixture();
  const auto nested = cache("/Series/Book") + "/nested/pixels";
  put(nested, "disposable");
  mutation_test::failPath = nested;
  EXPECT_EQ(BookFolderMutation::remove("/Series"), Result::RecoveryPending);
  EXPECT_FALSE(std::filesystem::exists(root / "Series"));
  EXPECT_EQ(get(nested), "disposable");
  EXPECT_NE(get("/.crosspoint/recent.json").find("/Series/Book"), std::string::npos);
  EXPECT_EQ(mutation_test::handles, 0);
  mutation_test::failPath.clear();
  EXPECT_EQ(recoverPending(), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / cache("/Series/Book").substr(1)));
}
TEST_F(MutationTest, StagingCleanupAcceptsLastOrdinalAndRetainsImpossibleOrdinal) {
  uint8_t marker[24]{};
  memcpy(marker, "CMI1", 4);
  bookmutation::put64(marker + 4, 123);
  bookmutation::put32(marker + 20, bookmutation::crc32(marker, 20));
  const std::string markerBytes(reinterpret_cast<char*>(marker), sizeof(marker));
  put("/.crosspoint/manga-mutation/staging.bin", markerBytes);
  put("/.crosspoint/manga-mutation/file-1023.stage", "owned");
  EXPECT_EQ(recoverPending(), Result::MutationFailed);  // Prepared content mutation never began.
  EXPECT_FALSE(std::filesystem::exists(root / ".crosspoint/manga-mutation"));
  EXPECT_FALSE(hasPending());
  put("/.crosspoint/manga-mutation/staging.bin", markerBytes);
  put("/.crosspoint/manga-mutation/file-1024.stage", "retain");
  EXPECT_EQ(recoverPending(), Result::RecoveryPending);
  EXPECT_EQ(get("/.crosspoint/manga-mutation/file-1024.stage"), "retain");
  EXPECT_EQ(get("/.crosspoint/manga-mutation/staging.bin"), markerBytes);
  EXPECT_TRUE(hasPending());
  EXPECT_EQ(mutation_test::handles, 0);
}
TEST_F(MutationTest, RestartsAcrossEveryMovePublicationPhase) {
  for (int phase : {2, 3, 4, 5, 6, 7, 8, 9}) {
    SCOPED_TRACE(phase);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    mutation_test::reset(root.string());
    recoverPending();
    fixture();
    mutation_test::cutPhase = phase;
    EXPECT_EQ(move("/Series", "/Moved"), Result::RecoveryPending);
    EXPECT_EQ(mutation_test::handles, 0);
    mutation_test::offline = false;
    mutation_test::cutPhase = 0;
    EXPECT_EQ(recoverPending(), Result::Complete);
    EXPECT_EQ(get(cache("/Moved/Book") + "/stats_v6.bin"), std::string(26429, 's'));
    EXPECT_EQ(get(cache("/Moved/Book") + "/dictionary.bin"), "route");
    EXPECT_FALSE(hasPending());
    EXPECT_EQ(mutation_test::handles, 0);
  }
}
TEST_F(MutationTest, FalsePhysicalRenameWithDestinationTokenRecoversForward) {
  fixture();
  mutation_test::cutAfterRename = true;
  EXPECT_EQ(move("/Series", "/Moved"), Result::RecoveryPending);
  mutation_test::offline = false;
  mutation_test::cutAfterRename = false;
  EXPECT_EQ(recoverPending(), Result::Complete);
  EXPECT_TRUE(std::filesystem::exists(root / "Moved/Book/panels.idx"));
}
TEST_F(MutationTest, TerminalJournalRetainedUntilOwnerReloadSucceeds) {
  fixture();
  mutation_test::failReload = true;
  EXPECT_EQ(move("/Series", "/Moved"), Result::RecoveryPending);
  EXPECT_TRUE(hasPending());
  EXPECT_TRUE(std::filesystem::exists(root / ".crosspoint/manga-mutation.finalize"));
  EXPECT_FALSE(std::filesystem::exists(root / ".crosspoint/manga-mutation"));
  mutation_test::failReload = false;
  EXPECT_EQ(recoverPending(), Result::Complete);
  EXPECT_FALSE(hasPending());
}
TEST_F(MutationTest, PartialDeleteCleansAbsentBookAndRetainsSurvivor) {
  fixture();
  put("/Series/Survivor/panels.idx", "x");
  put(cache("/Series/Survivor") + "/dictionary.bin", "keep");
  put("/.crosspoint/recent.json",
      "{\"books\":[{\"path\":\"/Series/Book\"},{\"path\":\"/Series/Survivor\"}],\"future\":1}");
  mutation_test::partialDeleteChild = "/Series/Book";
  EXPECT_EQ(BookFolderMutation::remove("/Series"), Result::MutationFailed);
  EXPECT_FALSE(std::filesystem::exists(root / "Series/Book"));
  EXPECT_TRUE(std::filesystem::exists(root / "Series/Survivor/panels.idx"));
  EXPECT_EQ(get(cache("/Series/Survivor") + "/dictionary.bin"), "keep");
  EXPECT_EQ(get("/.crosspoint/recent.json"), "{\"books\":[{\"path\":\"/Series/Survivor\"}],\"future\":1}");
}
TEST_F(MutationTest, InterruptedDeleteNeverDeletesRemainingContentOnReplay) {
  fixture();
  mutation_test::cutPhase = int(bookmutation::Phase::ContentSucceeded);
  EXPECT_EQ(BookFolderMutation::remove("/Series"), Result::RecoveryPending);
  put("/Series/Book/panels.idx", "recreated");
  mutation_test::offline = false;
  mutation_test::cutPhase = 0;
  EXPECT_EQ(recoverPending(), Result::MutationFailed);
  EXPECT_EQ(get("/Series/Book/panels.idx"), "recreated");
  EXPECT_EQ(get(cache("/Series/Book") + "/dictionary.bin"), "route");
  EXPECT_NE(get("/.crosspoint/recent.json").find("/Series/Book"), std::string::npos);
}
TEST_F(MutationTest, UppercaseIndexStillMigratesState) {
  fixture();
  std::filesystem::rename(root / "Series/Book/panels.idx", root / "Series/Book/PANELS.IDX");
  EXPECT_EQ(move("/Series", "/Moved"), Result::Complete);
  EXPECT_EQ(get(cache("/Moved/Book") + "/dictionary.bin"), "route");
}
TEST_F(MutationTest, PreservesRecoveryStatsAndRefusesSeventeenthDurableFile) {
  fixture();
  put(cache("/Series/Book") + "/stats_v6.bin.bak", "only valid backup");
  EXPECT_EQ(move("/Series", "/Moved"), Result::Complete);
  EXPECT_EQ(get(cache("/Moved/Book") + "/stats_v6.bin.bak"), "only valid backup");
  for (int i = 0; i < 17; ++i) put(cache("/Moved/Book") + "/stats_v" + std::to_string(100 + i) + ".bin", "future");
  EXPECT_EQ(move("/Moved", "/Again"), Result::StorageError);
  EXPECT_TRUE(std::filesystem::exists(root / "Moved/Book"));
}
TEST_F(MutationTest, DestinationRecoveryStateAndRecentIdentityNeverClobbered) {
  fixture();
  put(cache("/Moved/Book") + "/stats_v6.bin.bak", "unrelated");
  EXPECT_EQ(move("/Series", "/Moved"), Result::Collision);
  EXPECT_EQ(get(cache("/Moved/Book") + "/stats_v6.bin.bak"), "unrelated");
  std::filesystem::remove_all(root / (cache("/Moved/Book").substr(1)));
  put("/.crosspoint/recent.json", "{\"books\":[{\"path\":\"/Moved/Book\"}]}");
  EXPECT_EQ(move("/Series", "/Moved"), Result::Collision);
  EXPECT_TRUE(std::filesystem::exists(root / "Series/Book"));
}

TEST_F(MutationTest, CrcValidManifestCannotRedirectToUnrelatedMetadata) {
  fixture();
  mutation_test::cutPhase = int(bookmutation::Phase::Moved);
  ASSERT_EQ(move("/Series", "/Moved"), Result::RecoveryPending);
  mutation_test::offline = false;
  mutation_test::cutPhase = 0;
  const auto unrelated = get("/.crosspoint/state.json");
  auto bytes = get("/.crosspoint/manga-mutation/journal.bin");
  auto* p = reinterpret_cast<uint8_t*>(bytes.data());
  bookmutation::Header header;
  ASSERT_TRUE(bookmutation::decodeHeader(p, header));
  size_t at = 48;
  while (p[at] != 3) at += 8 + bookmutation::u32(p + at + 4);
  const size_t length = bookmutation::u16(p + at + 8 + 4);
  const std::string redirected = "/.crosspoint/state.json";
  ASSERT_GE(length, redirected.size());
  std::string replacement = redirected + std::string(length - redirected.size(), 'x');
  bytes.replace(at + 8 + 32, length, replacement);
  header.payloadCrc = bookmutation::crc32(p + 48, header.payloadBytes);
  bookmutation::encodeHeader(header, p);
  put("/.crosspoint/manga-mutation/journal.bin", bytes);
  EXPECT_EQ(recoverPending(), Result::RecoveryPending);
  EXPECT_EQ(get("/.crosspoint/state.json"), unrelated);
  EXPECT_EQ(get(cache("/Series/Book") + "/dictionary.bin"), "route");
  EXPECT_TRUE(hasPending());
}
TEST_F(MutationTest, UnknownTerminalMarkerRetainsEvidence) {
  fixture();
  put("/.crosspoint/manga-mutation.finalize", "unrecognized journal");
  EXPECT_EQ(recoverPending(), Result::RecoveryPending);
  EXPECT_EQ(get("/.crosspoint/manga-mutation.finalize"), "unrecognized journal");
  EXPECT_EQ(get(cache("/Series/Book") + "/dictionary.bin"), "route");
}
TEST_F(MutationTest, EverySingleIoFailureRetainsWholeDurableState) {
  fixture();
  mutation_test::calls = 0;
  ASSERT_EQ(move("/Series", "/Moved"), Result::Complete);
  const int total = mutation_test::calls;
  for (int cut = 1; cut <= total; ++cut) {
    SCOPED_TRACE(cut);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    mutation_test::reset(root.string());
    recoverPending();
    fixture();
    mutation_test::calls = 0;
    mutation_test::failAt = cut;
    const auto result = move("/Series", "/Moved");
    (void)result;
    mutation_test::failAt = 0;
    recoverPending();
    const bool old = std::filesystem::exists(root / "Series/Book/panels.idx"),
               moved = std::filesystem::exists(root / "Moved/Book/panels.idx");
    EXPECT_NE(old, moved);
    bool stats = get(cache("/Series/Book") + "/stats_v6.bin") == std::string(26429, 's') ||
                 get(cache("/Moved/Book") + "/stats_v6.bin") == std::string(26429, 's');
    EXPECT_TRUE(stats);
    EXPECT_EQ(mutation_test::handles, 0);
  }
  std::cout << "Injected single I/O failures: " << total << "\n";
}

TEST_F(MutationTest, CacheClearKeepsDictionaryAndAllWholeStatsRecoverySiblings) {
  fixture();
  const auto path = cache("/Series/Book");
  put(path + "/stats_v6.bin.bak", "sole backup");
  put(path + "/stats_v27.bin.recovery", "future");
  put(path + "/thumb_v3.bmp", "disposable");
  put(path + "/pixels/page.bin", "pixels");
  ASSERT_TRUE(bookmutation::clearMangaDisposableCache(path));
  EXPECT_EQ(get(path + "/stats_v6.bin"), std::string(26429, 's'));
  EXPECT_EQ(get(path + "/stats_v6.bin.bak"), "sole backup");
  EXPECT_EQ(get(path + "/stats_v27.bin.recovery"), "future");
  EXPECT_EQ(get(path + "/dictionary.bin"), "route");
  EXPECT_EQ(get(path + "/dictionary_history.txt"), "history");
  EXPECT_FALSE(std::filesystem::exists(root / (path + "/pixels").substr(1)));
}

TEST_F(MutationTest, DirectMovePreservesProgressAndSeparateBookmarkFamilies) {
  fixture();
  const char* old = "/Series/Book";
  const char* next = "/Direct";
  char path[128];
  for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
    snprintf(path, sizeof(path), "/.crosspoint/manga-state/%lu%s",
             static_cast<unsigned long>(bookmutation::bookPathHash(old)), suffix);
    put(path, std::string("progress") + suffix);
  }
  std::string bookmark(3, '\0');
  bookmark[0] = 5;
  bookmark[1] = 1;
  for (const std::string& field : {std::string("Title"), std::string("Author"), std::string(old)}) {
    uint8_t length[4];
    bookmutation::put32(length, field.size());
    bookmark.append(reinterpret_cast<char*>(length), 4);
    bookmark += field;
  }
  const std::string records(2 + 4 + 4 + BOOKMARK_CHAPTER_TITLE_MAX + 2 + BOOKMARK_SNIPPET_MAX, 'r');
  bookmark += records;
  for (bool legacy : {false, true}) {
    ASSERT_TRUE(BookmarkStore::folderMutationPath(old, legacy, path, sizeof(path)));
    put(path, bookmark);
  }
  ASSERT_EQ(move(old, next), Result::Complete);
  for (bool legacy : {false, true}) {
    ASSERT_TRUE(BookmarkStore::folderMutationPath(next, legacy, path, sizeof(path)));
    const auto migrated = get(path);
    EXPECT_NE(migrated.find(next), std::string::npos);
    EXPECT_EQ(migrated.substr(migrated.size() - records.size()), records);
  }
  for (const char* suffix : {".bin", ".bin.bak", ".bin.tmp"}) {
    snprintf(path, sizeof(path), "/.crosspoint/manga-state/%lu%s",
             static_cast<unsigned long>(bookmutation::bookPathHash(next)), suffix);
    EXPECT_EQ(get(path), std::string("progress") + suffix);
  }
}
TEST_F(MutationTest, DeleteDeduplicatesAncestorAndDuplicateRootsBeforeSnapshot) {
  fixture();
  const char* roots[] = {"/Series/Book", "/Series", "/Series"};
  EXPECT_EQ(removeMany(roots, 3), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / "Series"));
}
TEST_F(MutationTest, UsbDepthAndSnapshotLimitsAbortBeforeMutation) {
  fixture();
  put("/Series/a/b/c/d/e/f/g/h/i/panels.idx", "too deep");
  EXPECT_EQ(BookFolderMutation::remove("/Series", 8), Result::SnapshotLimit);
  EXPECT_TRUE(std::filesystem::exists(root / "Series/Book/panels.idx"));
  for (int i = 0; i < 65; ++i) put("/Series/folder" + std::to_string(i) + "/panels.idx", "manga");
  EXPECT_EQ(move("/Series", "/Moved"), Result::SnapshotLimit);
  EXPECT_EQ(get(cache("/Series/Book") + "/dictionary.bin"), "route");
}
TEST_F(MutationTest, UsbAcceptsFilesAndEmptyDirectoriesAtDepthEight) {
  put("/Series/a/b/c/d/e/f/g/h/panels.idx", "manga at depth eight");
  put("/Series/a/b/c/d/e/f/g/h/page.jpg", "pixels");
  std::filesystem::create_directories(root / "Series/a/b/c/d/e/f/g/empty");
  EXPECT_EQ(BookFolderMutation::remove("/Series", 8), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / "Series"));
  EXPECT_EQ(mutation_test::handles, 0);
}

TEST_F(MutationTest, TerminalReplayRejectsChangedFinalOutputsWithoutWrites) {
  for (const std::string& changed : {std::string("/.crosspoint/state.json"), std::string("/.crosspoint/recent.json"),
                                     cache("/Moved/Book") + "/dictionary.bin"}) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    mutation_test::reset(root.string());
    recoverPending();
    fixture();
    mutation_test::failReload = true;
    ASSERT_EQ(move("/Series", "/Moved"), Result::RecoveryPending);
    mutation_test::failReload = false;
    put(changed, "third-version");
    const auto journal = get("/.crosspoint/manga-mutation.finalize");
    EXPECT_EQ(recoverPending(), Result::RecoveryPending);
    EXPECT_EQ(get(changed), "third-version");
    EXPECT_EQ(get("/.crosspoint/manga-mutation.finalize"), journal);
  }
}
TEST_F(MutationTest, UppercaseDurableCacheNamesSurviveClearAndMove) {
  fixture();
  const auto old = cache("/Series/Book");
  std::filesystem::rename(root / (old + "/dictionary.bin").substr(1), root / (old + "/DICTIONARY.BIN").substr(1));
  put(old + "/STATS_V6.BIN.BAK", "sole valid uppercase backup");
  ASSERT_TRUE(bookmutation::clearMangaDisposableCache(old));
  EXPECT_EQ(get(old + "/DICTIONARY.BIN"), "route");
  ASSERT_EQ(move("/Series", "/Moved"), Result::Complete);
  EXPECT_EQ(get(cache("/Moved/Book") + "/DICTIONARY.BIN"), "route");
  EXPECT_EQ(get(cache("/Moved/Book") + "/STATS_V6.BIN.BAK"), "sole valid uppercase backup");
}
TEST_F(MutationTest, DeletesAnkiCacheAfterContentAbsenceAndRetainsUnrelatedCache) {
  put("/Cards/deck.cdeck", "deck");
  const char* book = "/Cards/deck.cdeck";
  uint64_t hash = 14695981039346656037ULL;
  for (const auto* p = reinterpret_cast<const unsigned char*>(book); *p; ++p) {
    hash ^= *p;
    hash *= 1099511628211ULL;
  }
  char cache[96];
  snprintf(cache, sizeof(cache), "/.crosspoint/anki_%016llx", static_cast<unsigned long long>(hash));
  put(std::string(cache) + "/progress.bin", "progress");
  put("/.crosspoint/anki_unrelated/keep", "retain");
  EXPECT_EQ(BookFolderMutation::remove("/Cards"), Result::Complete);
  EXPECT_FALSE(std::filesystem::exists(root / std::string(cache + 1)));
  EXPECT_EQ(get("/.crosspoint/anki_unrelated/keep"), "retain");
}
TEST_F(MutationTest, OrdinaryDeleteRetainsExistingXtcTxtCachePolicy) {
  put("/Books/story.txt", "text");
  put("/Books/pictures.xtc", "xtc");
  for (const char* type : {"txt", "xtc"}) {
    const char* book = !strcmp(type, "txt") ? "/Books/story.txt" : "/Books/pictures.xtc";
    char path[128];
    snprintf(path, sizeof(path), "/.crosspoint/bookmarks/%s_%lu.bin", type,
             static_cast<unsigned long>(bookmutation::bookPathHash(book)));
    put(path, "bookmark");
    snprintf(path, sizeof(path), "/.crosspoint/%s_%lu/keep", type,
             static_cast<unsigned long>(bookmutation::bookPathHash(book)));
    put(path, "keep");
  }
  EXPECT_EQ(BookFolderMutation::remove("/Books"), Result::Complete);
  for (const char* type : {"txt", "xtc"}) {
    const char* book = !strcmp(type, "txt") ? "/Books/story.txt" : "/Books/pictures.xtc";
    char path[128];
    snprintf(path, sizeof(path), "/.crosspoint/bookmarks/%s_%lu.bin", type,
             static_cast<unsigned long>(bookmutation::bookPathHash(book)));
    EXPECT_TRUE(get(path).empty());
    snprintf(path, sizeof(path), "/.crosspoint/%s_%lu/keep", type,
             static_cast<unsigned long>(bookmutation::bookPathHash(book)));
    EXPECT_EQ(get(path), "keep");
  }
}
}  // namespace
