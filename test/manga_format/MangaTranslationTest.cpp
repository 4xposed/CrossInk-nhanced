#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MangaTranslationPager.h"

namespace {
void u16(std::vector<uint8_t>& b, uint16_t n) {
  b.push_back(n);
  b.push_back(n >> 8);
}
manga::format::PageView translations(std::vector<uint8_t>& bytes, std::initializer_list<std::string> texts) {
  u16(bytes, texts.size());
  for (const auto& s : texts) {
    for (int i = 0; i < 4; ++i) u16(bytes, 0);
    u16(bytes, 0);
    u16(bytes, s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  manga::format::PageView page;
  EXPECT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  return page;
}
void collect(void* p, const char* glyph, int, int) { *static_cast<std::string*>(p) += glyph; }
}  // namespace
TEST(MangaTranslation, PagesStoredTranslationWithoutOcrAndCanRevisit) {
  std::vector<uint8_t> bytes;
  auto page = translations(bytes, {"abcdefghij", "第二"});
  std::string out;
  auto result = renderMangaTranslationPage(page, 0, 0, 3, 2, collect, &out);
  EXPECT_EQ(out, "abcdef");
  EXPECT_TRUE(result.more);
  EXPECT_FALSE(result.empty);
  EXPECT_FALSE(result.error);
  out.clear();
  result = renderMangaTranslationPage(page, 0, 1, 3, 2, collect, &out);
  EXPECT_EQ(out, "ghij");
  EXPECT_FALSE(result.more);
  out.clear();
  renderMangaTranslationPage(page, 0, 0, 3, 2, collect, &out);
  EXPECT_EQ(out, "abcdef");
  out.clear();
  renderMangaTranslationPage(page, 1, 0, 3, 2, collect, &out);
  EXPECT_EQ(out, "第二");
}
TEST(MangaTranslation, OverviewPreservesPanelsUtf8AndSanitizesControls) {
  std::vector<uint8_t> bytes;
  auto page = translations(bytes, {std::string("A\0B", 3), "猫𐐀"});
  std::string out;
  auto result = renderMangaTranslationPage(page, -1, 0, 8, 4, collect, &out);
  EXPECT_EQ(out, "A B猫𐐀");
  EXPECT_FALSE(result.more);
  EXPECT_FALSE(result.error);
}
TEST(MangaTranslation, EmptyInvalidAndLongTranslationAreBounded) {
  std::vector<uint8_t> bytes;
  auto page = translations(bytes, {"", std::string(12000, 'x')});
  EXPECT_TRUE(renderMangaTranslationPage(page, 0, 0, 8, 4, nullptr, nullptr).empty);
  EXPECT_TRUE(renderMangaTranslationPage(page, 3, 0, 8, 4, nullptr, nullptr).error);
  std::string out;
  auto result = renderMangaTranslationPage(page, 1, 374, 8, 4, collect, &out);
  EXPECT_EQ(out.size(), 32u);
  EXPECT_FALSE(result.more);
}

TEST(MangaTranslation, MalformedUtf8NeverSplitsOrReadsBeyondBorrowedText) {
  std::vector<uint8_t> bytes;
  auto page = translations(bytes, {std::string("\xf0\x90", 2), std::string("\xed\xa0\x80", 3), "x\r\ny"});
  std::string out;
  auto result = renderMangaTranslationPage(page, -1, 0, 10, 8, collect, &out);
  EXPECT_EQ(out, "?????xy");
  EXPECT_FALSE(result.error);
  EXPECT_FALSE(result.more);
}
TEST(MangaTranslation, ExactPageBoundaryDoesNotPublishAnEmptyNextPage) {
  std::vector<uint8_t> bytes;
  auto page = translations(bytes, {"123456\n", ""});
  std::string out;
  auto result = renderMangaTranslationPage(page, -1, 0, 3, 2, collect, &out);
  EXPECT_EQ(out, "123456");
  EXPECT_FALSE(result.more);
  EXPECT_TRUE(renderMangaTranslationPage({}, -1, 0, 3, 2, nullptr, nullptr).empty);
  EXPECT_TRUE(renderMangaTranslationPage(page, -2, 0, 3, 2, nullptr, nullptr).error);
  EXPECT_TRUE(renderMangaTranslationPage(page, 0, 0, 0, 2, nullptr, nullptr).error);
}
