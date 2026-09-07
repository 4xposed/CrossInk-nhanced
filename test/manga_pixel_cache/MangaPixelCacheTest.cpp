#include <HalStorage.h>
#include <MangaPixelCache.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <uzlib.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace manga {
std::string cachePath(const std::string& folder) {
  return "/.crosspoint/manga_" + std::to_string(uzlib_crc32(folder.data(), static_cast<unsigned>(folder.size()), 0));
}
}  // namespace manga

class PixelCacheTest : public ::testing::Test {
 protected:
  fs::path root;
  manga::PixelIdentity identity{};

  void SetUp() override {
    char pattern[] = "/tmp/crossink-pixels-XXXXXX";
    root = mkdtemp(pattern);
    storage_test::root = root;
    storage_test::failWrite = storage_test::failSync = storage_test::failRename = false;
    storage_test::openFiles = 0;
    storage_test::failRenameCall = storage_test::renameCalls = 0;
    identity.sourcePathCrc = 1;
    identity.sourceCrc = 2;
    identity.sourceSize = 8;
    identity.sourceWidth = 4096;
    identity.sourceHeight = 1536;
    identity.width = 5;
    identity.height = 2;
    identity.x = 7;
    identity.y = 9;
    identity.screenWidth = 800;
    identity.screenHeight = 480;
    identity.orientation = 1;
    identity.flags = manga::PixelPolicyGrayscale;
  }
  void TearDown() override {
    EXPECT_EQ(storage_test::openFiles, 0);
    fs::remove_all(root);
  }
  fs::path mapped(const char* path) { return storage_test::mapped(path); }
  void writeRaw(const char* path, uint16_t width = 5, uint16_t height = 2) {
    std::ofstream f(mapped(path), std::ios::binary);
    const uint8_t data[] = {
        uint8_t(width), uint8_t(width >> 8), uint8_t(height), uint8_t(height >> 8), 0x1b, 0x40, 0xe4, 0x80};
    f.write(reinterpret_cast<const char*>(data), sizeof(data));
  }
};

TEST_F(PixelCacheTest, FingerprintDetectsSameSizeReplacementAndPreservesGeometry) {
  const auto source = root / "page.jpg";
  std::ofstream(source, std::ios::binary).write("abcdefgh", 8);
  manga::PixelIdentity found = identity;
  ASSERT_TRUE(manga::fingerprintImage(source.c_str(), found));
  const auto first = found.sourceCrc;
  EXPECT_EQ(found.width, identity.width);
  std::ofstream(source, std::ios::binary | std::ios::trunc).write("ABCDEFGH", 8);
  ASSERT_TRUE(manga::fingerprintImage(source.c_str(), found));
  EXPECT_NE(found.sourceCrc, first);
  EXPECT_EQ(found.sourceSize, 8u);
}

TEST_F(PixelCacheTest, PublishesOddWidthRowsAndRewinds) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 12, -1, identity));
  EXPECT_EQ(std::string(cache.temporaryPath()), manga::cachePath("/books/a") + "/pixels_v1_p12_-1.pxc.tmp");
  writeRaw(cache.temporaryPath());
  ASSERT_TRUE(cache.publish());
  ASSERT_TRUE(cache.open());
  uint8_t row[2]{};
  ASSERT_TRUE(cache.readRow(row, sizeof(row)));
  EXPECT_EQ(row[0], 0x1b);
  EXPECT_EQ(row[1], 0x40);
  EXPECT_FALSE(cache.readRow(row, 1));
  ASSERT_TRUE(cache.rewind());
  ASSERT_TRUE(cache.readRow(row, sizeof(row)));
  EXPECT_EQ(row[0], 0x1b);
}

TEST_F(PixelCacheTest, ReadsValidatedSourceIdentityWithoutOpeningPayload) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 13, 3, identity));
  writeRaw(cache.temporaryPath());
  ASSERT_TRUE(cache.publish());
  manga::PixelIdentity found{};
  ASSERT_TRUE(manga::MangaPixelCache::sourceIdentity("/books/a", 13, 3, found));
  EXPECT_EQ(found.sourcePathCrc, identity.sourcePathCrc);
  EXPECT_EQ(found.sourceCrc, identity.sourceCrc);
  EXPECT_EQ(found.sourceSize, identity.sourceSize);
  EXPECT_EQ(found.sourceWidth, identity.sourceWidth);
  EXPECT_EQ(found.sourceHeight, identity.sourceHeight);
  EXPECT_EQ(found.width, identity.width);
  EXPECT_EQ(found.height, identity.height);
  EXPECT_EQ(storage_test::openFiles, 0);
}

TEST_F(PixelCacheTest, SourceIdentityRejectsBadMagicVersionDimensionsAndTruncation) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 14, 4, identity));
  const std::string temporary = cache.temporaryPath();
  const fs::path sidecar = mapped((temporary.substr(0, temporary.size() - 4) + ".id").c_str());
  auto publish = [&] {
    writeRaw(cache.temporaryPath());
    ASSERT_TRUE(cache.publish());
  };
  auto overwriteByte = [&](std::streamoff offset, uint8_t value) {
    std::fstream file(sidecar, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(offset);
    file.put(static_cast<char>(value));
  };
  manga::PixelIdentity found{};

  publish();
  overwriteByte(0, 'X');
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/books/a", 14, 4, found));
  EXPECT_EQ(storage_test::openFiles, 0);

  publish();
  overwriteByte(4, 2);
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/books/a", 14, 4, found));

  publish();
  overwriteByte(24, 0);
  overwriteByte(25, 0);
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/books/a", 14, 4, found));

  publish();
  fs::resize_file(sidecar, 47);
  EXPECT_FALSE(manga::MangaPixelCache::sourceIdentity("/books/a", 14, 4, found));
  EXPECT_EQ(storage_test::openFiles, 0);
}

TEST_F(PixelCacheTest, RejectsIdentityMismatchTruncationAndPayloadCorruptionBeforeRows) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 1, 2, identity));
  writeRaw(cache.temporaryPath());
  ASSERT_TRUE(cache.publish());
  manga::PixelIdentity changed = identity;
  changed.sourceCrc++;
  manga::MangaPixelCache mismatch;
  ASSERT_TRUE(mismatch.configure("/books/a", 1, 2, changed));
  EXPECT_FALSE(mismatch.open());
  auto raw = mapped(cache.temporaryPath());
  raw.replace_extension("");
  std::fstream corrupt(raw, std::ios::binary | std::ios::in | std::ios::out);
  corrupt.seekp(5);
  corrupt.put(char(0xff));
  corrupt.close();
  EXPECT_FALSE(cache.open());
  writeRaw(cache.temporaryPath());
  ASSERT_TRUE(cache.publish());
  fs::resize_file(raw, 7);
  EXPECT_FALSE(cache.open());
}

TEST_F(PixelCacheTest, RejectsCompletedTemporaryWithWrongRawDimensions) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 2, 0, identity));
  writeRaw(cache.temporaryPath(), 6, 2);
  EXPECT_FALSE(cache.publish());
  EXPECT_FALSE(fs::exists(mapped(cache.temporaryPath())));
  EXPECT_FALSE(cache.open());
}

TEST_F(PixelCacheTest, FailedPublishCleansTempsAndPublishedPairAndCanRetry) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 4, 0, identity));
  writeRaw(cache.temporaryPath());
  storage_test::failSync = true;
  EXPECT_FALSE(cache.publish());
  EXPECT_FALSE(fs::exists(mapped(cache.temporaryPath())));
  storage_test::failSync = false;
  writeRaw(cache.temporaryPath());
  storage_test::failRename = true;
  EXPECT_FALSE(cache.publish());
  storage_test::failRename = false;
  writeRaw(cache.temporaryPath());
  EXPECT_TRUE(cache.publish());
  EXPECT_TRUE(cache.open());
  cache.close();
}

TEST_F(PixelCacheTest, SecondRenameFailureLeavesNoOpenablePair) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 5, 1, identity));
  writeRaw(cache.temporaryPath());
  storage_test::failRenameCall = 2;
  EXPECT_FALSE(cache.publish());
  storage_test::failRenameCall = 0;
  EXPECT_FALSE(cache.open());
  EXPECT_EQ(storage_test::openFiles, 0);
}

TEST_F(PixelCacheTest, FailedConfigureClearsPathsWithoutDeletingPriorTemporary) {
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 6, 1, identity));
  writeRaw(cache.temporaryPath());
  const fs::path prior = mapped(cache.temporaryPath());
  ASSERT_TRUE(fs::exists(prior));
  EXPECT_FALSE(cache.configure("/books/a", 10000, 1, identity));
  EXPECT_STREQ(cache.temporaryPath(), "");
  cache.discardTemporary();
  EXPECT_TRUE(fs::exists(prior));
}

TEST_F(PixelCacheTest, RejectsUnknownPolicyAndOutOfBoundsGeometry) {
  manga::MangaPixelCache cache;
  identity.flags = 0x80;
  EXPECT_FALSE(cache.configure("/books/a", 0, 0, identity));
  identity.flags = 0;
  identity.x = 799;
  EXPECT_FALSE(cache.configure("/books/a", 0, 0, identity));
  identity.x = 0;
  identity.sourceWidth = 4096;
  identity.sourceHeight = 3072;
  EXPECT_FALSE(cache.configure("/books/a", 0, 0, identity));
  identity.sourceWidth = 4097;
  identity.sourceHeight = 100;
  EXPECT_FALSE(cache.configure("/books/a", 0, 0, identity));
  identity.sourceWidth = 100;
  identity.sourceHeight = 3073;
  EXPECT_FALSE(cache.configure("/books/a", 0, 0, identity));
  identity.sourceHeight = 100;
  EXPECT_FALSE(cache.configure("/books/a", 10000, 0, identity));
  EXPECT_FALSE(cache.configure("/books/a", 0, 255, identity));
}

namespace {
struct CancelAfterChecks {
  unsigned left;
  static bool check(void* p) {
    auto& state = *static_cast<CancelAfterChecks*>(p);
    if (state.left == 0) return true;
    --state.left;
    return false;
  }
};
}  // namespace

TEST_F(PixelCacheTest, FingerprintCancellationPreservesIdentityAndClosesChunkReader) {
  const auto source = root / "large.jpg";
  std::ofstream(source, std::ios::binary).write(std::string(4096, 'a').data(), 4096);
  for (unsigned checks : {0u, 2u, 9u, 16u}) {
    CancelAfterChecks cancel{checks};
    auto found = identity;
    EXPECT_FALSE(manga::fingerprintImage(source.c_str(), found, {CancelAfterChecks::check, &cancel}));
    EXPECT_EQ(found.sourceCrc, identity.sourceCrc);
    EXPECT_EQ(found.sourceSize, identity.sourceSize);
    EXPECT_EQ(storage_test::openFiles, 0);
  }
}

TEST_F(PixelCacheTest, CancelledValidationClosesPayloadAndCancelledPublicationPreservesOldPair) {
  identity.width = 128;
  identity.height = 128;
  manga::MangaPixelCache cache;
  ASSERT_TRUE(cache.configure("/books/a", 9, 1, identity));
  auto writeLarge = [&] {
    std::ofstream file(mapped(cache.temporaryPath()), std::ios::binary);
    const char header[] = {char(128), 0, char(128), 0};
    file.write(header, 4);
    file.write(std::string(4096, 'x').data(), 4096);
  };
  writeLarge();
  ASSERT_TRUE(cache.publish());
  for (unsigned checks : {0u, 4u, 11u, 17u}) {
    CancelAfterChecks cancel{checks};
    EXPECT_FALSE(cache.open({CancelAfterChecks::check, &cancel}));
    EXPECT_EQ(storage_test::openFiles, 0);
    ASSERT_TRUE(cache.open());
    cache.close();
    writeLarge();
    cancel.left = checks;
    EXPECT_FALSE(cache.publish({CancelAfterChecks::check, &cancel}));
    EXPECT_EQ(storage_test::openFiles, 0);
    EXPECT_FALSE(fs::exists(mapped(cache.temporaryPath())));
    EXPECT_FALSE(fs::exists(
        mapped((std::string(cache.temporaryPath()).substr(0, std::string(cache.temporaryPath()).size() - 4) + ".id.tmp")
                   .c_str())));
    EXPECT_TRUE(cache.open());
    cache.close();
  }
}
