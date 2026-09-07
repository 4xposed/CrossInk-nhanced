#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
namespace fs = std::filesystem;
namespace allocation_test {
size_t failSize = std::numeric_limits<size_t>::max();
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (size == allocation_test::failSize) return nullptr;
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}
class CoverTest : public ::testing::Test {
 protected:
  fs::path root, bookDir;
  void SetUp() override {
    converter_test::calls = 0;
    storage_test::failWrite = storage_test::failSync = storage_test::failRename = false;
    allocation_test::failSize = std::numeric_limits<size_t>::max();
    char t[] = "/tmp/crossink-cover-XXXXXX";
    root = mkdtemp(t);
    storage_test::root = root;
    bookDir = root / "book";
    fs::create_directories(bookDir);
    fs::copy_file(MANGA_FIXTURE_DIR "/panels.idx", bookDir / "panels.idx");
    fs::copy_file(MANGA_FIXTURE_DIR "/panels.dat", bookDir / "panels.dat");
  }
  void TearDown() override { fs::remove_all(root); }
  void image(const char* n, size_t bytes = 8) {
    std::ofstream f(bookDir / n, std::ios::binary);
    for (size_t i = 0; i < bytes; i++) f.put(char(i));
  }
  void bmp() {
    std::ofstream f(bookDir / "page_0000.bmp", std::ios::binary);
    BmpHeader header;
    createBmpHeader(&header, 4, 4, BmpRowOrder::TopDown);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));
    const uint8_t rows[16] = {0x00, 0, 0, 0, 0xff, 0, 0, 0, 0x00, 0, 0, 0, 0xff, 0, 0, 0};
    f.write(reinterpret_cast<const char*>(rows), sizeof(rows));
  }
  fs::path mapped(const std::string& p) { return storage_test::mapped(p.c_str()); }
};
TEST_F(CoverTest, PathsAreVersionedAndZeroWidthUsesTwoThirdsHeight) {
  EXPECT_EQ(manga::thumbnailTemplatePath("/books/a"), manga::cachePath("/books/a") + "/thumb_v3_[WIDTH]x[HEIGHT].bmp");
  EXPECT_EQ(manga::thumbnailPath("/books/a", 0, 300), manga::cachePath("/books/a") + "/thumb_v3_200x300.bmp");
}
TEST_F(CoverTest, MissingCoverAndInvalidDimensionsFail) {
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 120, 180));
  image("page_0000.JPG");
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 801, 180));
}
TEST_F(CoverTest, JpegIsConvertedAndValidCacheIsReused) {
  image("page_0000.JpG");
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  bool regenerated = false;
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 12, 18, &regenerated));
  EXPECT_TRUE(regenerated);
  auto p = mapped(manga::thumbnailPath(bookDir.string(), 12, 18));
  auto first = fs::last_write_time(p);
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 12, 18, &regenerated));
  EXPECT_FALSE(regenerated);
  EXPECT_EQ(first, fs::last_write_time(p));
  EXPECT_EQ(converter_test::calls, 1);
  std::ifstream f(p, std::ios::binary);
  char h[30]{};
  f.read(h, sizeof h);
  EXPECT_EQ((unsigned char)h[18], 12);
  EXPECT_EQ((unsigned char)h[22], 0xee);
}
TEST_F(CoverTest, TruncatedCacheIsRegeneratedAndSourceSizeInvalidatesIdentity) {
  image("page_0000.png", 8);
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  auto p = mapped(manga::thumbnailPath(bookDir.string(), 16, 24));
  fs::resize_file(p, 63);
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  EXPECT_GT(fs::file_size(p), 63u);
  image("page_0000.png", 20);
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  EXPECT_EQ(converter_test::calls, 3);
}
TEST_F(CoverTest, SameSizeSourceReplacementInvalidatesIdentity) {
  image("page_0000.png", 8);
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  EXPECT_EQ(converter_test::calls, 1);
  std::ofstream replacement(bookDir / "page_0000.png", std::ios::binary | std::ios::trunc);
  for (int i = 0; i < 8; ++i) replacement.put(char(20 + i));
  replacement.close();
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  EXPECT_EQ(converter_test::calls, 2);
}
TEST_F(CoverTest, MalformedPixelOffsetIsRegenerated) {
  image("page_0000.jpg");
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  const auto path = mapped(manga::thumbnailPath(bookDir.string(), 16, 24));
  std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
  corrupt.seekp(10);
  const uint8_t wrongOffset[4] = {63, 0, 0, 0};
  corrupt.write(reinterpret_cast<const char*>(wrongOffset), sizeof(wrongOffset));
  corrupt.close();
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 16, 24));
  EXPECT_EQ(converter_test::calls, 2);
  std::ifstream repaired(path, std::ios::binary);
  repaired.seekg(10);
  uint8_t offset = 0;
  repaired.read(reinterpret_cast<char*>(&offset), 1);
  EXPECT_EQ(offset, sizeof(BmpHeader));
}
TEST_F(CoverTest, SourceAspectDimensionsFitRuntimeBoundsWithoutLetterboxing) {
  int width = 0, height = 0;
  ASSERT_TRUE(manga::fitThumbnailDimensions(1200, 600, 800, 480, width, height));
  EXPECT_EQ(width, 800);
  EXPECT_EQ(height, 400);
  ASSERT_TRUE(manga::fitThumbnailDimensions(600, 1200, 800, 480, width, height));
  EXPECT_EQ(width, 240);
  EXPECT_EQ(height, 480);
}
TEST_F(CoverTest, BmpInputIsScaledThroughBoundedRowsAndPreservesBlackPixels) {
  bmp();
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  ASSERT_TRUE(manga::generateThumbnail(b, bookDir.string(), 2, 2));
  std::ifstream f(mapped(manga::thumbnailPath(bookDir.string(), 2, 2)), std::ios::binary);
  f.seekg(sizeof(BmpHeader));
  uint8_t first = 0xff;
  f.read(reinterpret_cast<char*>(&first), 1);
  EXPECT_NE(first, 0xff);
  EXPECT_EQ(converter_test::calls, 0);
}
TEST_F(CoverTest, FailuresLeaveNoPublishedCache) {
  image("page_0000.jpg");
  manga::MangaBook b;
  ASSERT_TRUE(b.open(bookDir.c_str()));
  const auto cache = mapped(manga::thumbnailPath(bookDir.string(), 20, 30));
  storage_test::failWrite = true;
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 20, 30));
  storage_test::failWrite = false;
  storage_test::failSync = true;
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 20, 30));
  storage_test::failSync = false;
  storage_test::failRename = true;
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 20, 30));
  storage_test::failRename = false;
  allocation_test::failSize = bookDir.string().size() + 258;
  EXPECT_FALSE(manga::generateThumbnail(b, bookDir.string(), 20, 30));
  EXPECT_FALSE(fs::exists(cache));
}
