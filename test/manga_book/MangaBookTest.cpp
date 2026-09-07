#include <HalStorage.h>
#include <MangaBook.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <vector>

namespace allocation_test {
size_t failSize = SIZE_MAX, failAfter = SIZE_MAX, largest = 0;
bool fail(size_t size) {
  largest = std::max(largest, size);
  if (size == failSize || failAfter == 0) return true;
  if (failAfter != SIZE_MAX) --failAfter;
  return false;
}
void reset() {
  failSize = failAfter = SIZE_MAX;
  largest = 0;
}
}  // namespace allocation_test
// Intercept only the fallible boundary; production keeps the real Memory.h.
// Delegating to ordinary global new/new[] preserves matching delete and ASan ownership.
void* operator new(size_t size, const std::nothrow_t&) noexcept {
  if (allocation_test::fail(size)) return nullptr;
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (allocation_test::fail(size)) return nullptr;
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

namespace fs = std::filesystem;
using manga::MangaBook;
class MangaBookTest : public ::testing::Test {
 protected:
  fs::path dir;
  void SetUp() override {
    storage_test::reset();
    allocation_test::reset();
    char temp[] = "/tmp/crossink-manga-book-XXXXXX";
    dir = fs::path(mkdtemp(temp)) / "任意の名" / "nested";
    fs::create_directories(dir);
    fixture("panels.idx");
    fixture("panels.dat");
  }
  void TearDown() override {
    allocation_test::reset();
    EXPECT_EQ(storage_test::handles, 0);
    EXPECT_EQ(storage_test::implicitCloses, 0);
    EXPECT_EQ(storage_test::overlaps, 0);
    fs::remove_all(dir.parent_path().parent_path());
  }
  void fixture(const char* name, const char* target = nullptr) {
    fs::copy_file(fs::path(MANGA_FIXTURES) / name, dir / (target ? target : name),
                  fs::copy_options::overwrite_existing);
  }
  void write(const std::string& name, const std::vector<uint8_t>& data = {}) {
    std::ofstream file(dir / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
  }
};
TEST_F(MangaBookTest, OriginalWriterPagesAndOptionalMetadataRemainUsableAfterHandlesClose) {
  fixture("meta-language.bin", "meta.bin");
  fixture("toc.idx");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageCount(), 3u);
  EXPECT_EQ(storage_test::handles, 0);
  EXPECT_EQ(book.title(), "漫画");
  EXPECT_EQ(book.author(), "作者");
  EXPECT_EQ(book.language(), "ja");
  manga::format::IndexRecord record;
  ASSERT_TRUE(book.readPageInfo(0, record));
  EXPECT_EQ(record.imageWidth, 800u);
  EXPECT_EQ(record.imageHeight, 480u);
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  EXPECT_EQ(page.panels.remaining, 2u);
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  EXPECT_EQ(panel.translation, "Hello!\nWorld.");
  EXPECT_EQ(book.tocCount(), 3u);
  manga::format::TocEntryView chapter;
  ASSERT_TRUE(book.readTocEntry(0, chapter));
  EXPECT_EQ(chapter.title, "Cover");
  EXPECT_EQ(panel.translation, "Hello!\nWorld.");
  ASSERT_TRUE(book.loadPage(2, page));
  EXPECT_EQ(page.panels.remaining, 0u);
  book.close();
  EXPECT_TRUE(book.title().empty());
  EXPECT_EQ(book.pageCount(), 0u);
}
TEST_F(MangaBookTest, MarkerDetectionAndTrailingSlashDoNotDependOnNameOrDepth) {
  ASSERT_TRUE(MangaBook::isMangaFolder(dir.c_str()));
  EXPECT_FALSE(MangaBook::isMangaFolder(""));
  EXPECT_FALSE(MangaBook::isMangaFolder(nullptr));
  MangaBook book;
  ASSERT_TRUE(book.open((dir.string() + "///").c_str()));
  EXPECT_EQ(book.title(), "nested");
  auto deeper = dir;
  for (int i = 0; i < 20; ++i) deeper /= "long-subfolder-name";
  fs::create_directories(deeper);
  fs::copy_file(dir / "panels.idx", deeper / "panels.idx");
  fs::copy_file(dir / "panels.dat", deeper / "panels.dat");
  EXPECT_TRUE(MangaBook::isMangaFolder(deeper.c_str()));
  ASSERT_TRUE(book.open(deeper.c_str()));
  EXPECT_EQ(book.title(), "long-subfolder-name");
  fs::remove(deeper / "panels.idx");
  fs::create_directory(deeper / "panels.idx");
  EXPECT_FALSE(MangaBook::isMangaFolder(deeper.c_str()));
}
TEST_F(MangaBookTest, CanonicalDuplicatePriorityAndMixedInteriorKeepPhysicalIdentity) {
  for (const auto* name : {"page_0000.jpg", "page_0002.jpg", "page_0000.bmp", "page_0001.bmp", "page_0002.bmp"})
    write(name);
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024] = "stale";
  ASSERT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0000.jpg");
  ASSERT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0001.bmp");
  ASSERT_EQ(book.pageImagePath(2, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0002.jpg");
  EXPECT_EQ(book.pageImagePath(2, path, 5), manga::PathResult::Error);
  EXPECT_STREQ(path, "");
  EXPECT_EQ(book.pageImagePath(3, path, sizeof(path)), manga::PathResult::Error);
}
TEST_F(MangaBookTest, CanonicalBmpAndPngResolveWithoutDirectoryWalk) {
  for (const char* ext : {"bmp", "png"}) {
    for (int i = 0; i < 3; ++i) write("page_000" + std::to_string(i) + "." + ext);
    storage_test::failNext = dir.string();
    MangaBook book;
    ASSERT_TRUE(book.open(dir.c_str()));
    char path[1024];
    EXPECT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Found);
    EXPECT_EQ(fs::path(path).filename(), "page_0001." + std::string(ext));
    for (int i = 0; i < 3; ++i) fs::remove(dir / ("page_000" + std::to_string(i) + "." + ext));
  }
}
TEST_F(MangaBookTest, LegacyOrderingFiltersCropsAndKeepsPinnedDirectoryOrder) {
  for (const auto* name : {"10.JPG", "2.jpeg", "1.png", "a-cover.jpg", "z-COVER.bmp", "copyright.png", ".hidden.jpg",
                           "p0_0.jpg", "P12_333.bmp", "readme.txt"})
    write(name);
  fs::create_directory(dir / "0.jpg");
  // Increase only the index page count/records; empty pages require no data file.
  std::vector<uint8_t> idx(8 + 6 * 12);
  idx[0] = 2;
  idx[4] = 6;
  write("panels.idx", idx);
  std::vector<std::string> pinned;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (name == "a-cover.jpg" || name == "z-COVER.bmp") pinned.push_back(name);
  }
  ASSERT_EQ(pinned.size(), 2u);
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  const std::vector<std::string> expected{pinned[0], pinned[1], "copyright.png", "1.png", "2.jpeg", "10.JPG"};
  char path[1024];
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(book.pageImagePath(i, path, sizeof(path)), manga::PathResult::Found);
    EXPECT_EQ(fs::path(path).filename(), expected[i]);
  }
  ASSERT_EQ(book.pageImagePath(3, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "1.png");
}
TEST_F(MangaBookTest, PanelOnlyAndBothCropLayoutsPreferBmpWithPerCropJpegFallback) {
  write("p0_0.jpg");
  write("p0_0.bmp");
  write("p0_1.jpg");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024];
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Missing);
  ASSERT_EQ(book.panelImagePath(0, 0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "p0_0.bmp");
  ASSERT_EQ(book.panelImagePath(0, 1, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "p0_1.jpg");
  fs::create_directory(dir / "panels");
  write("panels/p0_0.jpg");
  write("panels/p0_1.bmp");
  ASSERT_TRUE(book.open(dir.c_str()));
  ASSERT_EQ(book.panelImagePath(0, 0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path), dir / "panels/p0_0.jpg");
  EXPECT_EQ(book.panelImagePath(0, 2, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_STREQ(path, "");
  fs::remove_all(dir / "panels");
  write("panels");
  ASSERT_TRUE(book.open(dir.c_str()));
  ASSERT_EQ(book.panelImagePath(0, 0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path), dir / "p0_0.bmp");
}
TEST_F(MangaBookTest, EveryIndexRecordIsValidatedBeforeOpenPublishesState) {
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  auto bytes = std::vector<uint8_t>(44);
  bytes[0] = 2;
  bytes[4] = 3;
  bytes[8 + 2 * 12] = 100;
  bytes[8 + 2 * 12 + 4] = 1;
  write("panels.idx", bytes);
  EXPECT_FALSE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageCount(), 0u);
  EXPECT_TRUE(book.title().empty());
  fixture("panels.idx");
  fs::resize_file(dir / "panels.idx", 43);
  EXPECT_FALSE(book.open(dir.c_str()));
  fixture("panels.idx");
  fs::remove(dir / "panels.dat");
  EXPECT_FALSE(book.open(dir.c_str()));
  EXPECT_FALSE(book.open(""));
  EXPECT_FALSE(book.open(nullptr));
}
TEST_F(MangaBookTest, EmptyRecordsIgnoreOffsetsAndAllowAbsentData) {
  std::vector<uint8_t> idx(20);
  idx[0] = 2;
  idx[4] = 1;
  for (size_t i = 8; i < 12; ++i) idx[i] = 255;
  idx[16] = idx[17] = 255;
  write("panels.idx", idx);
  fs::remove(dir / "panels.dat");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::IndexRecord info;
  ASSERT_TRUE(book.readPageInfo(0, info));
  EXPECT_EQ(info.dataOffset, UINT32_MAX);
  EXPECT_EQ(info.imageWidth, UINT16_MAX);
  manga::format::PageView page;
  EXPECT_TRUE(book.loadPage(0, page));
  EXPECT_EQ(page.panels.remaining, 0);
}
TEST_F(MangaBookTest, OptionalLegacyMalformedOrMissingMetadataFallsBackWithoutPartialViews) {
  MangaBook book;
  fixture("meta-legacy.bin", "meta.bin");
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "漫画");
  EXPECT_EQ(book.author(), "作者");
  EXPECT_TRUE(book.language().empty());
  fixture("toc.idx");
  for (const auto* file : {"meta.bin", "toc.idx"}) fs::resize_file(dir / file, 9);
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "nested");
  EXPECT_TRUE(book.author().empty());
  EXPECT_EQ(book.tocCount(), 0u);
  fixture("meta-language.bin", "meta.bin");
  fs::resize_file(dir / "meta.bin", 21);  // One-byte incomplete language header.
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "nested");
}
TEST_F(MangaBookTest, OptionalFieldsPreserveFullEncodedWidthsAndOutOfRangeTocPages) {
  std::vector<uint8_t> meta(8 + 65535 * 3 + 2, 'x');
  meta[0] = 1;
  meta[1] = meta[2] = meta[3] = 0;
  meta[4] = meta[5] = meta[6] = meta[7] = 255;
  meta[8 + 65535 * 2] = meta[9 + 65535 * 2] = 255;
  write("meta.bin", meta);
  std::vector<uint8_t> toc(8 + 6 + 65535, 'y');
  toc[0] = 1;
  toc[1] = toc[2] = toc[3] = 0;
  toc[4] = 1;
  toc[5] = toc[6] = toc[7] = 0;
  for (size_t i = 8; i < 14; ++i) toc[i] = 255;
  write("toc.idx", toc);
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title().size(), 65535u);
  EXPECT_EQ(book.author().size(), 65535u);
  EXPECT_EQ(book.language().size(), 65535u);
  manga::format::TocEntryView entry;
  ASSERT_TRUE(book.readTocEntry(0, entry));
  EXPECT_EQ(entry.pageIndex, UINT32_MAX);
  EXPECT_EQ(entry.title, std::string(65535, 'y'));
}
TEST_F(MangaBookTest, ShortReadsSeeksAndCloseFailuresClearPageOutputsAndDoNotLeakHandles) {
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::PageView page;
  for (auto* fault :
       {&storage_test::failRead, &storage_test::failSeek, &storage_test::failClose, &storage_test::failOpen}) {
    ASSERT_TRUE(book.loadPage(0, page));
    *fault = (dir / "panels.dat").string();
    EXPECT_FALSE(book.loadPage(0, page));
    EXPECT_EQ(page.panels.remaining, 0u);
    EXPECT_TRUE(page.panels.bytes.empty());
    EXPECT_EQ(storage_test::handles, 0);
    fault->clear();
  }
  for (auto* fault : {&storage_test::failRead, &storage_test::failSeek, &storage_test::failClose}) {
    *fault = (dir / "panels.idx").string();
    manga::format::IndexRecord record{1, 1, 1, 1};
    EXPECT_FALSE(book.readPageInfo(0, record));
    EXPECT_EQ(record.dataLength, 0u);
    EXPECT_FALSE(book.loadPage(0, page));
    fault->clear();
  }
  EXPECT_FALSE(book.loadPage(999, page));
  EXPECT_EQ(page.panels.remaining, 0u);
  fs::resize_file(dir / "panels.dat", 20);
  EXPECT_FALSE(book.loadPage(0, page));
  EXPECT_TRUE(page.panels.bytes.empty());
}
TEST_F(MangaBookTest, OptionalIoFailuresFallBackAndTocReadFailuresClearOutputs) {
  fixture("meta-language.bin", "meta.bin");
  fixture("toc.idx");
  MangaBook book;
  for (const auto* filename : {"meta.bin", "toc.idx"}) {
    for (auto* fault :
         {&storage_test::failRead, &storage_test::failSeek, &storage_test::failClose, &storage_test::failOpen}) {
      *fault = (dir / filename).string();
      ASSERT_TRUE(book.open(dir.c_str()));
      if (std::string(filename) == "meta.bin") {
        EXPECT_EQ(book.title(), "nested");
        EXPECT_TRUE(book.author().empty());
      } else
        EXPECT_EQ(book.tocCount(), 0u);
      fault->clear();
    }
  }
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::TocEntryView entry;
  for (auto* fault :
       {&storage_test::failRead, &storage_test::failSeek, &storage_test::failClose, &storage_test::failOpen}) {
    ASSERT_TRUE(book.readTocEntry(1, entry));
    EXPECT_EQ(entry.title, "第一章");
    *fault = (dir / "toc.idx").string();
    EXPECT_FALSE(book.readTocEntry(1, entry));
    EXPECT_TRUE(entry.title.empty());
    EXPECT_EQ(entry.pageIndex, 0u);
    fault->clear();
  }
  EXPECT_FALSE(book.readTocEntry(3, entry));
  EXPECT_TRUE(entry.title.empty());
}
TEST_F(MangaBookTest, ImageDirectoryAndProbeFailuresAreErrorsNotPanelOnlyBooks) {
  write("1.jpg");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024];
  for (auto* fault : {&storage_test::failNext, &storage_test::failClose, &storage_test::failOpen}) {
    *fault = dir.string();
    EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Error);
    EXPECT_STREQ(path, "");
    fault->clear();
  }
  storage_test::failClose = (dir / "1.jpg").string();
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Error);
  storage_test::failClose.clear();
  write("p0_0.jpg");
  storage_test::failOpen = (dir / "p0_0.jpg").string();
  EXPECT_EQ(book.panelImagePath(0, 0, path, sizeof(path)), manga::PathResult::Error);
  storage_test::failOpen.clear();
  EXPECT_EQ(book.pageImagePath(0, nullptr, 0), manga::PathResult::Error);
}
TEST_F(MangaBookTest, FailedFilenameReadCannotShiftLegacyPageIndexes) {
  write("1.jpg");
  write("2.jpg");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  storage_test::failName = (dir / "1.jpg").string();
  char path[1024];
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Error);
  EXPECT_STREQ(path, "");
  EXPECT_EQ(storage_test::handles, 0);
}
TEST_F(MangaBookTest, FallibleRequiredBuffersFailOpenAndOptionalBuffersFallBack) {
  fixture("meta-language.bin", "meta.bin");
  fixture("toc.idx");
  MangaBook book;
  for (size_t failAfter : {0u, 1u, 2u}) {  // folder, path scratch, reusable page.
    allocation_test::failAfter = failAfter;
    EXPECT_FALSE(book.open(dir.c_str()));
    EXPECT_EQ(book.pageCount(), 0u);
    EXPECT_TRUE(book.title().empty());
    EXPECT_EQ(storage_test::handles, 0);
    allocation_test::reset();
  }
  allocation_test::failSize = 24;  // Original metadata extent.
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "nested");
  EXPECT_TRUE(book.author().empty());
  EXPECT_EQ(book.tocCount(), 3u);
  allocation_test::reset();
  allocation_test::failSize = 9;  // Longest original chapter label in UTF-8 bytes.
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "漫画");
  EXPECT_EQ(book.tocCount(), 0u);
  allocation_test::reset();
  ASSERT_TRUE(book.open(dir.c_str()));
  write("1.jpg");
  allocation_test::failAfter = 0;
  char path[1024];
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Error);
  EXPECT_STREQ(path, "");
}
TEST_F(MangaBookTest, OptionalTrailingFileSizeDoesNotControlAllocation) {
  fixture("meta-language.bin", "meta.bin");
  fixture("toc.idx");
  fs::resize_file(dir / "meta.bin", 1024ull * 1024 * 1024);
  fs::resize_file(dir / "toc.idx", 1024ull * 1024 * 1024);
  allocation_test::reset();
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.title(), "漫画");
  EXPECT_EQ(book.language(), "ja");
  EXPECT_EQ(book.tocCount(), 3u);
  EXPECT_LT(allocation_test::largest, 1024u);
  manga::format::TocEntryView chapter;
  EXPECT_TRUE(book.readTocEntry(2, chapter));
  EXPECT_EQ(chapter.title, "第二章");
}
TEST_F(MangaBookTest, ThousandMaximumLabelsAreStreamedWithOneReusableLabelBuffer) {
  std::ofstream toc(dir / "toc.idx", std::ios::binary);
  const uint8_t header[]{1, 0, 0, 0, 0xe8, 3, 0, 0};
  toc.write(reinterpret_cast<const char*>(header), 8);
  for (uint32_t i = 0; i < 1000; ++i) {
    const uint8_t entry[]{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 0, 0, 255, 255};
    toc.write(reinterpret_cast<const char*>(entry), 6);
    toc.seekp(65534, std::ios::cur);
    toc.put('z');
  }
  toc.close();
  allocation_test::reset();
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.tocCount(), 1000u);
  EXPECT_EQ(allocation_test::largest, 65535u);
  manga::format::TocEntryView chapter;
  ASSERT_TRUE(book.readTocEntry(999, chapter));
  EXPECT_EQ(chapter.pageIndex, 999u);
  ASSERT_EQ(chapter.title.size(), 65535u);
  EXPECT_EQ(chapter.title.back(), 'z');
  ASSERT_TRUE(book.readTocEntry(0, chapter));
  EXPECT_EQ(chapter.pageIndex, 0u);
}
TEST_F(MangaBookTest, SequentialTocReadsResumeAtTheCachedNextRecord) {
  const std::vector<uint8_t> toc = {
      1, 0, 0, 0, 3, 0, 0,   0,  // header
      0, 0, 0, 0, 1, 0, 'A',     // entry 0
      1, 0, 0, 0, 1, 0, 'B',     // entry 1
      2, 0, 0, 0, 1, 0, 'C',     // entry 2
  };
  write("toc.idx", toc);
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::TocEntryView chapter;
  ASSERT_TRUE(book.readTocEntry(0, chapter));
  EXPECT_EQ(chapter.title, "A");

  // Damage the already-consumed record. A sequential read must start from the
  // cached byte offset, while a backwards read must rescan and see the damage.
  std::fstream file(dir / "toc.idx", std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(12);
  const uint8_t invalidLength[]{255, 255};
  file.write(reinterpret_cast<const char*>(invalidLength), sizeof(invalidLength));
  file.close();

  ASSERT_TRUE(book.readTocEntry(1, chapter));
  EXPECT_EQ(chapter.pageIndex, 1u);
  EXPECT_EQ(chapter.title, "B");
  EXPECT_FALSE(book.readTocEntry(0, chapter));
  EXPECT_TRUE(chapter.title.empty());
}
TEST_F(MangaBookTest, TocCursorSupportsBackwardsRandomReadsAndRecoversAfterFailure) {
  fixture("toc.idx");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::TocEntryView chapter;
  ASSERT_TRUE(book.readTocEntry(2, chapter));
  EXPECT_EQ(chapter.title, "第二章");
  ASSERT_TRUE(book.readTocEntry(0, chapter));
  EXPECT_EQ(chapter.title, "Cover");

  storage_test::failRead = (dir / "toc.idx").string();
  EXPECT_FALSE(book.readTocEntry(1, chapter));
  EXPECT_TRUE(chapter.title.empty());
  storage_test::failRead.clear();
  ASSERT_TRUE(book.readTocEntry(1, chapter));
  EXPECT_EQ(chapter.title, "第一章");
}
TEST_F(MangaBookTest, LegacyComparatorPinsCombinedCoverCopyrightAndUsesOrdinalForNaturalTies) {
  for (const char* name : {"cover.jpg", "copyright.jpg", "cover-copyright.jpg", "01.jpg", "1.JPG"}) write(name);
  std::vector<uint8_t> idx(8 + 5 * 12);
  idx[0] = 2;
  idx[4] = 5;
  write("panels.idx", idx);
  std::vector<std::string> ties;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (name == "01.jpg" || name == "1.JPG") ties.push_back(name);
  }
  ASSERT_EQ(ties.size(), 2u);
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024];
  const std::vector<std::string> expected{"cover-copyright.jpg", "cover.jpg", "copyright.jpg", ties[0], ties[1]};
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(book.pageImagePath(i, path, sizeof(path)), manga::PathResult::Found);
    EXPECT_EQ(fs::path(path).filename(), expected[i]);
  }
}
TEST_F(MangaBookTest, ReopenDropsCachedPathsAndInvalidatesFailedReadResults) {
  write("1.jpg");
  MangaBook book;
  char path[1024];
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(book.open(dir.c_str()));
    EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
    EXPECT_EQ(fs::path(path).filename(), "1.jpg");
    manga::format::PageView page;
    ASSERT_TRUE(book.loadPage(0, page));
    EXPECT_EQ(page.panels.remaining, 2u);
    book.close();
    EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Error);
    EXPECT_STREQ(path, "");
    EXPECT_FALSE(book.loadPage(0, page));
    EXPECT_EQ(page.panels.remaining, 0u);
  }
  fs::remove(dir / "1.jpg");
  write("2.bmp");
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "2.bmp");
  storage_test::failClose = (dir / "panels.idx").string();
  EXPECT_FALSE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageCount(), 0u);
  EXPECT_TRUE(book.title().empty());
}
TEST_F(MangaBookTest, DataExtentsAndSeeksUse64BitsWhilePreservingEncodedOffset) {
  std::vector<uint8_t> idx(20);
  idx[0] = 2;
  idx[4] = 1;
  idx[8] = idx[9] = idx[10] = idx[11] = 255;
  idx[12] = 2;
  write("panels.idx", idx);
  std::ofstream data(dir / "panels.dat", std::ios::binary);
  data.seekp(UINT32_MAX);
  data.put(0);
  data.put(0);
  data.close();
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::IndexRecord info;
  ASSERT_TRUE(book.readPageInfo(0, info));
  EXPECT_EQ(info.dataOffset, UINT32_MAX);
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  EXPECT_EQ(page.panels.remaining, 0u);
}
TEST_F(MangaBookTest, PageCorruptionAndIndexGrowthAfterOpenCannotExposePartialPageOrGrowBuffer) {
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  auto data = std::vector<uint8_t>(89, 0);
  data[0] = 255;
  write("panels.dat", data);
  EXPECT_FALSE(book.loadPage(0, page));
  EXPECT_EQ(page.panels.remaining, 0u);
  EXPECT_TRUE(page.panels.bytes.empty());
  auto idx = std::vector<uint8_t>(44, 0);
  idx[0] = 2;
  idx[4] = 3;
  idx[12] = 89;
  write("panels.idx", idx);
  allocation_test::failAfter = 0;
  EXPECT_FALSE(book.loadPage(0, page));
  EXPECT_TRUE(page.panels.bytes.empty());
}

TEST_F(MangaBookTest, LightweightModesSkipReaderBuffersAndOptionalCoverMetadata) {
  fixture("meta-language.bin", "meta.bin");
  fixture("toc.idx");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::IndexRecord record;
  ASSERT_TRUE(book.readPageInfo(0, record));
  book.close();
  allocation_test::failSize = record.dataLength;
  ASSERT_TRUE(book.open(dir.c_str(), manga::OpenMode::Metadata));
  EXPECT_EQ(book.title(), "漫画");
  EXPECT_EQ(book.pageCount(), 3u);
  EXPECT_EQ(book.tocCount(), 0u);
  manga::format::PageView page;
  EXPECT_FALSE(book.loadPage(0, page));
  ASSERT_TRUE(book.open(dir.c_str(), manga::OpenMode::Cover));
  EXPECT_EQ(book.title(), "nested");
  EXPECT_TRUE(book.author().empty());
  EXPECT_EQ(book.tocCount(), 0u);
  EXPECT_FALSE(book.loadPage(0, page));
  ASSERT_TRUE(book.open(dir.c_str(), manga::OpenMode::Index));
  EXPECT_EQ(book.pageCount(), 3u);
  EXPECT_EQ(book.title(), "nested");
  EXPECT_EQ(book.tocCount(), 0u);
  EXPECT_TRUE(book.author().empty());
  allocation_test::reset();
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_TRUE(book.loadPage(0, page));
}

TEST_F(MangaBookTest, BrowserSparseImagesNeverCompactHolesOrLoseOcrAlignment) {
  // Independent browser layout: retain cover and middle overview, omit last.
  std::vector<uint8_t> idx(8 + 4 * 12);
  idx[0] = 2;
  idx[4] = 4;
  // Keep original writer page-zero OCR record as independently encoded fixture.
  idx[12] = 87;
  idx[16] = 0x20;
  idx[17] = 3;
  idx[18] = 0xe0;
  idx[19] = 1;
  write("panels.idx", idx);
  write("page_0000.jpg");
  write("page_0002.png");
  fs::create_directory(dir / "panels");
  write("panels/p1_0.bmp");
  write("panels/p3_0.jpg");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024];
  ASSERT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0000.jpg");
  EXPECT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_STREQ(path, "");
  ASSERT_EQ(book.panelImagePath(1, 0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "p1_0.bmp");
  ASSERT_EQ(book.pageImagePath(2, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0002.png");
  EXPECT_EQ(book.pageImagePath(3, path, sizeof(path)), manga::PathResult::Missing);
  ASSERT_EQ(book.panelImagePath(3, 0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "p3_0.jpg");
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  EXPECT_EQ(panel.translation, "Hello!\nWorld.");
  ASSERT_TRUE(book.loadPage(2, page));
  EXPECT_EQ(page.panels.remaining, 0u);
}
TEST_F(MangaBookTest, BrowserCoverPlusCropsAndMixedJpegKeepIndexesWithoutDirectoryWalks) {
  write("page_0000.jpg");
  write("panels-placeholder.txt");
  write("p1_0.bmp");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  storage_test::failNext = dir.string();
  char path[1024];
  EXPECT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_EQ(book.pageImagePath(2, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_EQ(book.panelImagePath(1, 0, path, sizeof(path)), manga::PathResult::Found);
  write("page_0001.jpeg");
  write("page_0002.png");
  ASSERT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0001.jpeg");
  ASSERT_EQ(book.pageImagePath(2, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0002.png");
}

TEST_F(MangaBookTest, ExpiredOpenDoesNotReadIndexAndMidIndexCancellationClosesReader) {
  MangaBook book;
  CooperativeCancellation expired{[](void*) { return true; }, nullptr};
  EXPECT_FALSE(book.open(dir.c_str(), manga::OpenMode::Cover, expired));
  EXPECT_EQ(storage_test::readCalls, 0);
  CooperativeCancellation afterThreeReads{[](void*) { return storage_test::readCalls >= 3; }, nullptr};
  EXPECT_FALSE(book.open(dir.c_str(), manga::OpenMode::Cover, afterThreeReads));
  EXPECT_EQ(storage_test::readCalls, 3);
  EXPECT_EQ(storage_test::handles, 0);
  EXPECT_EQ(book.pageCount(), 0u);
}
TEST_F(MangaBookTest, CancelledLegacyDiscoveryStopsEnumerationAndCanRetry) {
  write("1.jpg");
  write("2.jpg");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str(), manga::OpenMode::Cover));
  storage_test::nextCalls = 0;
  CooperativeCancellation afterTwoEntries{[](void*) { return storage_test::nextCalls >= 2; }, nullptr};
  char path[1024];
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path), afterTwoEntries), manga::PathResult::Error);
  EXPECT_STREQ(path, "");
  EXPECT_EQ(storage_test::nextCalls, 2);
  EXPECT_EQ(storage_test::handles, 0);
  ASSERT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "1.jpg");
}
TEST_F(MangaBookTest, MissingCoverCanonicalClassificationPreservesRetainedMiddleIdentity) {
  write("page_0001.jpeg");
  write("p0_0.bmp");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  char path[1024];
  EXPECT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Missing);
  const int scanned = storage_test::nextCalls;
  ASSERT_GT(scanned, 0);
  ASSERT_EQ(book.pageImagePath(1, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0001.jpeg");
  EXPECT_EQ(book.pageImagePath(2, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_EQ(storage_test::nextCalls, scanned);
}

TEST_F(MangaBookTest, MokuroIndexLoadsJapaneseTextDimensionsAndCanonicalBitmap) {
  fs::remove(dir / "panels.idx");
  fs::remove(dir / "panels.dat");
  const std::vector<uint8_t> index = {
      'C', 'M', 'I', '1', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 19, 0, 0, 0, 0xe0, 1, 0x20, 3, 7, 0, 0, 0, 2, 0, 0, 0,
  };
  const std::vector<uint8_t> data = {
      1, 0, 0, 0, 10, 0, 20, 0, 30, 0, 40, 0, 3, 0, 1, 0, 0xe7, 0x8c, 0xab,
  };
  write("book.mki", index);
  write("book.mkd", data);
  write("page_0000.bmp");
  write("page_0000.jpg");
  write("p0_0.bmp");

  EXPECT_TRUE(MangaBook::isMangaFolder(dir.c_str()));
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageCount(), 1u);
  manga::format::IndexRecord info;
  ASSERT_TRUE(book.readPageInfo(0, info));
  EXPECT_EQ(info.dataOffset, 0u);
  EXPECT_EQ(info.dataLength, 19u);
  EXPECT_EQ(info.imageWidth, 480u);
  EXPECT_EQ(info.imageHeight, 800u);

  char path[1024];
  ASSERT_EQ(book.pageImagePath(0, path, sizeof(path)), manga::PathResult::Found);
  EXPECT_EQ(fs::path(path).filename(), "page_0000.bmp");
  EXPECT_EQ(book.panelImagePath(0, 0, path, sizeof(path)), manga::PathResult::Missing);
  EXPECT_STREQ(path, "");

  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  ASSERT_EQ(page.panels.remaining, 1u);
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  EXPECT_EQ(panel.box.x, 0u);
  EXPECT_EQ(panel.box.y, 0u);
  EXPECT_EQ(panel.box.w, 480u);
  EXPECT_EQ(panel.box.h, 800u);
  EXPECT_TRUE(panel.translation.empty());
  ASSERT_EQ(panel.texts.remaining, 1u);
  manga::format::TextView text;
  ASSERT_EQ(panel.texts.next(text), manga::format::Error::None);
  EXPECT_EQ(text.box.x, 10u);
  EXPECT_EQ(text.box.y, 20u);
  EXPECT_EQ(text.box.w, 30u);
  EXPECT_EQ(text.box.h, 40u);
  EXPECT_EQ(text.text, "猫");
}

TEST_F(MangaBookTest, MokuroEmptyOcrRecordStillPresentsOneFullImagePanel) {
  fs::remove(dir / "panels.idx");
  fs::remove(dir / "panels.dat");
  const std::vector<uint8_t> index = {
      'C', 'M', 'I', '1', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0xe0, 1, 0x20, 3, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  ASSERT_EQ(index.size(), 32u);
  write("book.mki", index);
  write("book.mkd", {0, 0, 0, 0});
  write("page_0000.bmp");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  ASSERT_EQ(page.panels.remaining, 1u);
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  EXPECT_EQ(panel.box.w, 480u);
  EXPECT_EQ(panel.box.h, 800u);
  EXPECT_EQ(panel.texts.remaining, 0u);
}

TEST_F(MangaBookTest, MokuroVersionTwoSupportsX3AndKeepsVersionOneBounds) {
  std::vector<uint8_t> index = {
      'C', 'M', 'I', '1', 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0x10, 2, 0x18, 3, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  write("book.mki", index);
  write("book.mkd", {0, 0, 0, 0});
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  EXPECT_EQ(panel.box.w, 528u);
  EXPECT_EQ(panel.box.h, 792u);
  book.close();
  index[4] = 1;
  write("book.mki", index);
  EXPECT_FALSE(book.open(dir.c_str()));
  index[4] = 2;
  index[20] = 0x11;
  write("book.mki", index);
  EXPECT_FALSE(book.open(dir.c_str()));
}

TEST_F(MangaBookTest, MokuroOpenRejectsMalformedIndexRecordsAndExactSizeViolations) {
  fs::remove(dir / "panels.idx");
  fs::remove(dir / "panels.dat");
  const std::vector<uint8_t> valid = {
      'C', 'M', 'I', '1', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 19, 0, 0, 0, 0xe0, 1, 0x20, 3, 7, 0, 0, 0, 2, 0, 0, 0,
  };
  write("book.mkd", {1, 0, 0, 0, 10, 0, 20, 0, 30, 0, 40, 0, 3, 0, 1, 0, 0xe7, 0x8c, 0xab});
  MangaBook book;
  const auto rejected = [&](std::vector<uint8_t> index) {
    write("book.mki", index);
    EXPECT_FALSE(book.open(dir.c_str()));
    EXPECT_EQ(book.pageCount(), 0u);
    EXPECT_TRUE(book.title().empty());
  };
  auto invalid = valid;
  invalid[0] = 'X';
  rejected(invalid);
  invalid = valid;
  invalid[4] = 3;
  rejected(invalid);
  invalid = valid;
  invalid[8] = 0;
  rejected(invalid);
  invalid = valid;
  invalid[20] = invalid[21] = 0;
  rejected(invalid);
  invalid = valid;
  invalid[20] = 0xe1;
  invalid[21] = 1;
  rejected(invalid);
  invalid = valid;
  invalid[22] = 0x21;
  invalid[23] = 3;
  rejected(invalid);
  invalid = valid;
  invalid[16] = 3;
  rejected(invalid);
  invalid = valid;
  invalid[16] = 20;
  rejected(invalid);
  invalid = valid;
  invalid[12] = 1;
  invalid[16] = 18;
  rejected(invalid);
  invalid = valid;
  invalid[31] = 1;
  rejected(invalid);
  invalid = valid;
  invalid.pop_back();
  rejected(invalid);
  invalid = valid;
  invalid.push_back(0);
  rejected(invalid);
}

TEST_F(MangaBookTest, MokuroMalformedDataClearsAPreviouslyPublishedPageView) {
  fs::remove(dir / "panels.idx");
  fs::remove(dir / "panels.dat");
  const std::vector<uint8_t> index = {
      'C', 'M', 'I', '1', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 19, 0, 0, 0, 0xe0, 1, 0x20, 3, 7, 0, 0, 0, 2, 0, 0, 0,
  };
  const std::vector<uint8_t> valid = {
      1, 0, 0, 0, 10, 0, 20, 0, 30, 0, 40, 0, 3, 0, 1, 0, 0xe7, 0x8c, 0xab,
  };
  write("book.mki", index);
  write("book.mkd", valid);
  write("page_0000.bmp");
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  manga::format::PageView page;
  const auto rejected = [&](std::vector<uint8_t> data) {
    write("book.mkd", valid);
    ASSERT_TRUE(book.loadPage(0, page));
    EXPECT_EQ(page.panels.remaining, 1u);
    write("book.mkd", data);
    EXPECT_FALSE(book.loadPage(0, page));
    EXPECT_EQ(page.panels.remaining, 0u);
    EXPECT_TRUE(page.panels.bytes.empty());
  };
  auto invalid = valid;
  invalid[14] = 2;
  rejected(invalid);
  invalid = valid;
  invalid[4] = 0xd6;
  invalid[5] = 1;
  rejected(invalid);
  invalid = valid;
  invalid[8] = invalid[9] = 0;
  rejected(invalid);
  invalid = valid;
  invalid[16] = 0;
  rejected(invalid);
  invalid = valid;
  invalid[16] = 0xe0;
  invalid[17] = 0x80;
  invalid[18] = 0x80;
  rejected(invalid);
  invalid = valid;
  invalid[0] = 2;
  rejected(invalid);
  invalid = valid;
  invalid.pop_back();
  rejected(invalid);
}

TEST_F(MangaBookTest, MokuroMaximumRecordReusesOneBoundedPageAllocation) {
  fs::remove(dir / "panels.idx");
  fs::remove(dir / "panels.dat");
  const std::vector<uint8_t> index = {
      'C', 'M', 'I', '1', 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xf2, 0x7f, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  std::vector<uint8_t> data(32754, 'a');
  const uint8_t header[] = {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0xe2, 0x7f, 0, 0};
  std::copy(std::begin(header), std::end(header), data.begin());
  write("book.mki", index);
  write("book.mkd", data);

  // A second raw-record allocation would match failSize and fail this open.
  // The one owned buffer includes the 14-byte adapter prefix instead.
  allocation_test::failSize = 32754;
  MangaBook book;
  ASSERT_TRUE(book.open(dir.c_str()));
  EXPECT_EQ(allocation_test::largest, 32768u);
  manga::format::PageView page;
  ASSERT_TRUE(book.loadPage(0, page));
  manga::format::PanelView panel;
  ASSERT_EQ(page.panels.next(panel), manga::format::Error::None);
  manga::format::TextView text;
  ASSERT_EQ(panel.texts.next(text), manga::format::Error::None);
  EXPECT_EQ(text.text.size(), 32738u);

  allocation_test::reset();
  allocation_test::failSize = 32768;
  EXPECT_FALSE(book.open(dir.c_str()));
  EXPECT_EQ(book.pageCount(), 0u);
}
