#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <vector>

#include "MangaFormat.h"

using namespace manga::format;
namespace {
std::vector<uint8_t> fixture(const char* name) {
  std::ifstream stream(std::string(MANGA_FIXTURES) + "/" + name, std::ios::binary);
  EXPECT_TRUE(stream.good());
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
}  // namespace

TEST(MangaFormat, OriginalWriterIndexAndPagesPreserveCoordinatesAndText) {
  const auto idx = fixture("panels.idx");
  const auto dat = fixture("panels.dat");
  IndexHeader header;
  ASSERT_EQ(decodeIndex(idx, dat.size(), header), Error::None);
  ASSERT_EQ(header.pageCount, 3u);
  IndexRecord record;
  ASSERT_EQ(decodeIndexRecord(Bytes(idx).subspan(8), dat.size(), record), Error::None);
  EXPECT_EQ(record.imageWidth, 800);
  EXPECT_EQ(record.imageHeight, 480);
  PageView page;
  ASSERT_EQ(decodePage(Bytes(dat).subspan(record.dataOffset, record.dataLength), page), Error::None);
  ASSERT_EQ(page.panels.remaining, 2);
  PanelView panel;
  ASSERT_EQ(page.panels.next(panel), Error::None);
  EXPECT_EQ(panel.box.x, 600);
  EXPECT_EQ(panel.box.y, 20);
  EXPECT_EQ(panel.box.w, 190);
  EXPECT_EQ(panel.box.h, 450);
  EXPECT_EQ(panel.translation, "Hello!\nWorld.");
  EXPECT_EQ(panel.texts.remaining, 2);
  TextView text;
  ASSERT_EQ(panel.texts.next(text), Error::None);
  EXPECT_EQ(text.box.x, 12);
  EXPECT_EQ(text.box.y, 30);
  EXPECT_EQ(text.box.w, 70);
  EXPECT_EQ(text.box.h, 200);
  EXPECT_EQ(text.text, "こんにちは世界");
  ASSERT_EQ(panel.texts.next(text), Error::None);
  EXPECT_EQ(text.text, std::string_view("猫\0犬", 7));
  EXPECT_EQ(text.box.w, 0);
  EXPECT_EQ(panel.texts.next(text), Error::End);
  EXPECT_TRUE(text.text.empty());
  ASSERT_EQ(page.panels.next(panel), Error::None);
  EXPECT_EQ(panel.box.w, 0);
  EXPECT_TRUE(panel.translation.empty());
  EXPECT_EQ(page.panels.next(panel), Error::End);
  EXPECT_TRUE(panel.translation.empty());
  ASSERT_EQ(decodeIndexRecord(Bytes(idx).subspan(20), dat.size(), record), Error::None);
  EXPECT_EQ(record.imageWidth, 65535);
  ASSERT_EQ(decodePage(Bytes(dat).subspan(record.dataOffset, record.dataLength), page), Error::None);
  EXPECT_EQ(page.panels.remaining, 0);
  ASSERT_EQ(decodeIndexRecord(Bytes(idx).subspan(32), dat.size(), record), Error::None);
  EXPECT_EQ(record.dataLength, 0u);
  EXPECT_EQ(record.imageHeight, 65535);
  EXPECT_EQ(decodePage({}, page), Error::None);
}

TEST(MangaFormat, MetadataLegacyAndLanguageFromOriginalWriter) {
  MetaView meta;
  auto bytes = fixture("meta-language.bin");
  ASSERT_EQ(decodeMeta(bytes, meta), Error::None);
  EXPECT_EQ(meta.title, "漫画");
  EXPECT_EQ(meta.author, "作者");
  EXPECT_EQ(meta.language, "ja");
  bytes = fixture("meta-legacy.bin");
  ASSERT_EQ(decodeMeta(bytes, meta), Error::None);
  EXPECT_EQ(meta.title, "漫画");
  EXPECT_TRUE(meta.language.empty());
}

TEST(MangaFormat, OriginalWriterTocPreservesEntryOrder) {
  auto bytes = fixture("toc.idx");
  TocView toc;
  ASSERT_EQ(decodeToc(bytes, toc), Error::None);
  ASSERT_EQ(toc.entries.remaining, 3u);
  TocEntryView entry;
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, 0u);
  EXPECT_EQ(entry.title, "Cover");
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, 1u);
  EXPECT_EQ(entry.title, "第一章");
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, 2u);
  EXPECT_EQ(entry.title, "第二章");
  EXPECT_EQ(toc.entries.next(entry), Error::End);
  EXPECT_TRUE(entry.title.empty());
}

TEST(MangaFormat, EveryRequiredPrefixTruncationClearsOutputs) {
  auto idx = fixture("panels.idx");
  auto dat = fixture("panels.dat");
  for (size_t size = 0; size < idx.size(); ++size) {
    IndexHeader header{99};
    EXPECT_NE(decodeIndex(Bytes(idx).first(size), dat.size(), header), Error::None) << size;
    EXPECT_EQ(header.pageCount, 0u);
  }
  IndexRecord record;
  ASSERT_EQ(decodeIndexRecord(Bytes(idx).subspan(8), dat.size(), record), Error::None);
  for (size_t size = 1; size < record.dataLength; ++size) {
    PageView page{{{}, 99}};
    EXPECT_NE(decodePage(Bytes(dat).first(size), page), Error::None) << size;
    EXPECT_EQ(page.panels.remaining, 0);
    EXPECT_TRUE(page.panels.bytes.empty());
  }
  for (const char* filename : {"meta-legacy.bin", "toc.idx"}) {
    const auto bytes = fixture(filename);
    for (size_t size = 0; size < bytes.size(); ++size) {
      MetaView meta{"old", "old", "old"};
      TocView toc{{{}, 99}};
      if (filename[0] == 'm') {
        EXPECT_NE(decodeMeta(Bytes(bytes).first(size), meta), Error::None) << size;
        EXPECT_TRUE(meta.title.empty());
        EXPECT_TRUE(meta.author.empty());
        EXPECT_TRUE(meta.language.empty());
      } else {
        EXPECT_NE(decodeToc(Bytes(bytes).first(size), toc), Error::None) << size;
        EXPECT_EQ(toc.entries.remaining, 0u);
        EXPECT_TRUE(toc.entries.bytes.empty());
      }
    }
  }
}

TEST(MangaFormat, IndexRejectsVersionCountLengthAndOverflowingExtent) {
  auto idx = fixture("panels.idx");
  IndexHeader header;
  for (uint32_t version : {0u, 1u, 3u, UINT32_MAX}) {
    u32(idx, 0, version);
    EXPECT_EQ(decodeIndex(idx, 1000, header), Error::UnsupportedVersion);
  }
  u32(idx, 0, 2);
  for (uint32_t count : {0u, 10001u, UINT32_MAX}) {
    u32(idx, 4, count);
    EXPECT_EQ(decodeIndex(idx, 1000, header), Error::InvalidCount);
  }
  IndexRecord record{1, 1, 1, 1};
  std::vector<uint8_t> raw(12);
  u32(raw, 0, UINT32_MAX - 4);
  u32(raw, 4, 10);
  EXPECT_EQ(decodeIndexRecord(raw, UINT32_MAX, record), Error::DataExtent);
  EXPECT_EQ(record.dataOffset, 0u);
  u32(raw, 0, 0);
  u32(raw, 4, 32769);
  EXPECT_EQ(decodeIndexRecord(raw, UINT32_MAX, record), Error::PageTooLarge);
}

TEST(MangaFormat, FullUpstreamBoundariesAndEmptyTocAreAccepted) {
  std::vector<uint8_t> idx(8 + 10000 * 12);
  u32(idx, 0, 2);
  u32(idx, 4, 10000);
  IndexHeader header;
  ASSERT_EQ(decodeIndex(idx, 0, header), Error::None);
  EXPECT_EQ(header.pageCount, 10000u);
  std::vector<uint8_t> dat(32768, 0xff);
  dat[0] = 0;
  PageView page;
  ASSERT_EQ(decodePage(dat, page), Error::None);
  EXPECT_EQ(page.panels.remaining, 0);
  dat.push_back(0);
  EXPECT_EQ(decodePage(dat, page), Error::PageTooLarge);
  IndexRecord record;
  std::vector<uint8_t> raw(12);
  u32(raw, 0, UINT32_MAX);
  u32(raw, 4, 32768);
  EXPECT_EQ(decodeIndexRecord(raw, uint64_t(UINT32_MAX) + 32768, record), Error::None);
  EXPECT_EQ(record.dataOffset, UINT32_MAX);
  EXPECT_EQ(record.dataLength, 32768u);
  EXPECT_EQ(decodeIndexRecord(Bytes(raw).first(11), UINT64_MAX, record), Error::Truncated);
  EXPECT_EQ(record.dataLength, 0u);
  std::vector<uint8_t> tocBytes(8 + 1000 * 6);
  u32(tocBytes, 0, 1);
  u32(tocBytes, 4, 1000);
  u32(tocBytes, 8, UINT32_MAX);  // --max-pages can leave chapters beyond the book extent.
  TocView toc;
  ASSERT_EQ(decodeToc(tocBytes, toc), Error::None);
  EXPECT_EQ(toc.entries.remaining, 1000u);
  TocEntryView entry;
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, UINT32_MAX);
  u32(tocBytes, 4, 1001);
  EXPECT_EQ(decodeToc(tocBytes, toc), Error::InvalidCount);
  EXPECT_EQ(toc.entries.remaining, 0u);
  u32(tocBytes, 4, 0);
  ASSERT_EQ(decodeToc(tocBytes, toc), Error::None);
  EXPECT_EQ(toc.entries.next(entry), Error::End);
}

TEST(MangaFormat, MaximumPanelAndTextCountsDoNotWrap) {
  std::vector<uint8_t> bytes(2 + 255 * 12);
  bytes[0] = 255;
  PageView page;
  ASSERT_EQ(decodePage(bytes, page), Error::None);
  PanelView panel;
  for (size_t i = 0; i < 255; ++i) ASSERT_EQ(page.panels.next(panel), Error::None);
  EXPECT_EQ(page.panels.next(panel), Error::End);
  bytes.assign(2 + 12 + 255 * 10, 0);
  bytes[0] = 1;
  bytes[10] = 255;
  ASSERT_EQ(decodePage(bytes, page), Error::None);
  ASSERT_EQ(page.panels.next(panel), Error::None);
  TextView text;
  for (size_t i = 0; i < 255; ++i) ASSERT_EQ(panel.texts.next(text), Error::None);
  EXPECT_EQ(panel.texts.next(text), Error::End);
}

TEST(MangaFormat, FullLengthOptionalFieldsAndUnsortedTocRemainUnmodified) {
  std::vector<uint8_t> bytes(8 + 65535 * 3 + 2, 'x');
  u32(bytes, 0, 1);
  bytes[4] = bytes[5] = bytes[6] = bytes[7] = 255;
  bytes[8 + 65535 * 2] = bytes[9 + 65535 * 2] = 255;
  MetaView meta;
  ASSERT_EQ(decodeMeta(bytes, meta), Error::None);
  EXPECT_EQ(meta.title.size(), 65535u);
  EXPECT_EQ(meta.author.size(), 65535u);
  EXPECT_EQ(meta.language.size(), 65535u);
  bytes.assign(8 + 6 + 65535 + 6, 'x');
  u32(bytes, 0, 1);
  u32(bytes, 4, 2);
  u32(bytes, 8, 99);
  bytes[12] = bytes[13] = 255;
  u32(bytes, 14 + 65535, 0);
  bytes[18 + 65535] = bytes[19 + 65535] = 0;
  TocView toc;
  ASSERT_EQ(decodeToc(bytes, toc), Error::None);
  TocEntryView entry;
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, 99u);
  EXPECT_EQ(entry.title.size(), 65535u);
  ASSERT_EQ(toc.entries.next(entry), Error::None);
  EXPECT_EQ(entry.pageIndex, 0u);
  EXPECT_TRUE(entry.title.empty());
}

TEST(MangaFormat, MalformedOptionalFilesClearAllState) {
  auto bytes = fixture("meta-language.bin");
  const auto legacy = fixture("meta-legacy.bin");
  for (size_t size = legacy.size() + 1; size < bytes.size(); ++size) {
    MetaView meta{"stale", "stale", "stale"};
    EXPECT_EQ(decodeMeta(Bytes(bytes).first(size), meta), Error::Truncated);
    EXPECT_TRUE(meta.title.empty());
    EXPECT_TRUE(meta.author.empty());
    EXPECT_TRUE(meta.language.empty());
  }
  MetaView meta;
  u32(bytes, 0, 2);
  EXPECT_EQ(decodeMeta(bytes, meta), Error::UnsupportedVersion);
  bytes = fixture("toc.idx");
  TocView toc;
  u32(bytes, 0, 2);
  EXPECT_EQ(decodeToc(bytes, toc), Error::UnsupportedVersion);
}

TEST(MangaFormat, OversizedTextLengthsAreRejectedWithoutPartialPage) {
  const auto original = fixture("panels.dat");
  for (size_t offset : {12u, 35u}) {  // Translation length, then first OCR text length.
    auto bytes = original;
    bytes[offset] = bytes[offset + 1] = 255;
    PageView page{{{}, 9}};
    EXPECT_EQ(decodePage(bytes, page), Error::Truncated);
    EXPECT_EQ(page.panels.remaining, 0);
    EXPECT_TRUE(page.panels.bytes.empty());
  }
}

TEST(MangaFormat, UnalignedBuffersBorrowOriginalBytesAndIgnoreReservedExtensions) {
  auto dat = fixture("panels.dat");
  dat.insert(dat.begin(), 0xff);
  dat[2] = 0xff;   // Page reserved byte.
  dat[12] = 0xff;  // Panel reserved byte.
  dat.push_back(0xff);
  PageView page;
  ASSERT_EQ(decodePage(Bytes(dat).subspan(1), page), Error::None);
  PanelView panel;
  ASSERT_EQ(page.panels.next(panel), Error::None);
  EXPECT_EQ(panel.translation.data(), reinterpret_cast<const char*>(dat.data() + 15));
  TextView text;
  ASSERT_EQ(panel.texts.next(text), Error::None);
  EXPECT_EQ(text.text, "こんにちは世界");
  for (const char* name : {"panels.idx", "meta-language.bin", "toc.idx"}) {
    auto bytes = fixture(name);
    bytes.insert(bytes.begin(), 0xff);
    bytes.push_back(0xff);
    if (name[0] == 'p') {
      IndexHeader header;
      EXPECT_EQ(decodeIndex(Bytes(bytes).subspan(1), dat.size(), header), Error::None);
    } else if (name[0] == 'm') {
      MetaView meta;
      EXPECT_EQ(decodeMeta(Bytes(bytes).subspan(1), meta), Error::None);
      EXPECT_EQ(meta.language, "ja");
    } else {
      TocView toc;
      EXPECT_EQ(decodeToc(Bytes(bytes).subspan(1), toc), Error::None);
    }
  }
}

TEST(MangaFormat, FailedCursorsClearOutputAndDoNotAdvance) {
  const std::vector<uint8_t> emptyHeader(1);
  PanelCursor panels{emptyHeader, 1};
  PanelView panel{{1, 1, 1, 1}, "old", {{}, 1}};
  EXPECT_EQ(panels.next(panel), Error::Truncated);
  EXPECT_EQ(panels.remaining, 1);
  EXPECT_EQ(panels.bytes.size(), 1u);
  EXPECT_EQ(panel.box.x, 0);
  EXPECT_TRUE(panel.translation.empty());
  std::vector<uint8_t> malformedText(10);
  malformedText[8] = 1;
  TextCursor texts{malformedText, 1};
  TextView text{{1, 1, 1, 1}, "old"};
  EXPECT_EQ(texts.next(text), Error::Truncated);
  EXPECT_EQ(texts.remaining, 1);
  EXPECT_TRUE(text.text.empty());
  std::vector<uint8_t> malformedToc(6);
  malformedToc[4] = 1;
  TocCursor entries{malformedToc, 1};
  TocEntryView entry{9, "old"};
  EXPECT_EQ(entries.next(entry), Error::Truncated);
  EXPECT_EQ(entries.remaining, 1u);
  EXPECT_EQ(entry.pageIndex, 0u);
  EXPECT_TRUE(entry.title.empty());
  EXPECT_STREQ(errorName(Error::Truncated), "truncated record");
  EXPECT_STREQ(errorName(Error::DataExtent), "page record exceeds data extent");
}

TEST(MangaFormat, ZeroLengthPageIgnoresUnusedOffsetLikeUpstream) {
  std::vector<uint8_t> bytes(12);
  u32(bytes, 0, UINT32_MAX);
  IndexRecord record;
  ASSERT_EQ(decodeIndexRecord(bytes, 0, record), Error::None);
  EXPECT_EQ(record.dataOffset, UINT32_MAX);
  EXPECT_EQ(record.dataLength, 0u);
}
