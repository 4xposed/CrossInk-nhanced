#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "activities/anki/AnkiCardText.h"

namespace {

enum class RenderEvent {
  PrimaryScanDraw,
  PrimaryEndScanAndPrewarm,
  PrimaryRealDraw,
  NormalScanDraw,
  NormalEndScanAndPrewarm,
  NormalRealDraw,
  ScanDraw = NormalScanDraw,
  EndScanAndPrewarm = NormalEndScanAndPrewarm,
  RealDraw = NormalRealDraw,
};

struct RecordingFontCache {
  class PrewarmScope {
   public:
    explicit PrewarmScope(RecordingFontCache& cache) : cache_(cache) {}

    bool endScanAndPrewarm() {
      cache_.events.push_back(cache_.lastRole == FontRole::Primary ? RenderEvent::PrimaryEndScanAndPrewarm
                                                                   : RenderEvent::NormalEndScanAndPrewarm);
      cache_.scanning = false;
      return true;
    }

   private:
    RecordingFontCache& cache_;
  };

  PrewarmScope createPrewarmScope() {
    scanning = true;
    return PrewarmScope(*this);
  }

  bool scanning = false;
  FontRole lastRole = FontRole::Normal;
  std::vector<RenderEvent> events;
};

constexpr size_t kFieldStorageBytes = static_cast<size_t>(kMaxCardFields) * (kMaxCardFieldTextBytes + 1);

void setField(std::array<CardField, kMaxCardFields>& fields, char* storage, const uint8_t index, const char* text) {
  const size_t length = std::strlen(text);
  ASSERT_LE(length, kMaxCardFieldTextBytes);
  std::memcpy(storage + static_cast<size_t>(index) * (kMaxCardFieldTextBytes + 1), text, length + 1);
  fields[index].text = storage + static_cast<size_t>(index) * (kMaxCardFieldTextBytes + 1);
  fields[index].length = static_cast<uint16_t>(length);
}

TEST(AnkiCardText, FlattensJapaneseAndLatinAnswerBlocksForSingleFontPreparation) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  setField(fields, storage.data(), 0, "Japanese answer:");
  setField(fields, storage.data(), 1, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
  setField(fields, storage.data(), 2, "Latin explanation");

  FlattenedCardText flattened;
  ASSERT_TRUE(flattenCardFieldsInPlace(fields, 3, storage.data(), storage.size(), flattened));

  constexpr char expected[] = "Japanese answer:\n\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n\nLatin explanation";
  EXPECT_EQ(flattened.length, std::strlen(expected));
  EXPECT_EQ(flattened.text, storage.data() + storage.size() - std::strlen(expected) - 1);
  EXPECT_STREQ(flattened.text, expected);
}

TEST(AnkiCardText, PrewarmsFlattenedJapaneseAndLatinBeforeTheRealDraw) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  setField(fields, storage.data(), 0, "Japanese:");
  setField(fields, storage.data(), 1, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
  setField(fields, storage.data(), 2, "Latin explanation");

  FlattenedCardText flattened;
  ASSERT_TRUE(flattenCardFieldsInPlace(fields, 3, storage.data(), storage.size(), flattened));
  constexpr char expected[] = "Japanese:\n\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n\nLatin explanation";

  RecordingFontCache fontCache;
  std::vector<std::string> renderedText;
  std::vector<int> renderedFontIds;
  ASSERT_TRUE(renderCardTextWithPrewarm(fontCache, flattened.text, /*fontId=*/42,
                                        [&fontCache, &renderedText, &renderedFontIds](char* const text,
                                                                                       const int fontId) {
                                          fontCache.events.push_back(fontCache.scanning ? RenderEvent::ScanDraw
                                                                                         : RenderEvent::RealDraw);
                                          renderedText.emplace_back(text);
                                          renderedFontIds.push_back(fontId);
                                        }));

  ASSERT_EQ(fontCache.events.size(), 3U);
  EXPECT_EQ(fontCache.events[0], RenderEvent::ScanDraw);
  EXPECT_EQ(fontCache.events[1], RenderEvent::EndScanAndPrewarm);
  EXPECT_EQ(fontCache.events[2], RenderEvent::RealDraw);
  ASSERT_EQ(renderedText.size(), 2U);
  EXPECT_EQ(renderedText[0], expected);
  EXPECT_EQ(renderedText[1], expected);
  ASSERT_EQ(renderedFontIds.size(), 2U);
  EXPECT_EQ(renderedFontIds[0], 42);
  EXPECT_EQ(renderedFontIds[1], 42);
}

TEST(AnkiCardText, KeepsJapaneseAndLatinOnTheBaselineNormalFontWhenPrimaryLayoutDoesNotFit) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  setField(fields, storage.data(), 0, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
  fields[0].primary = true;
  setField(fields, storage.data(), 1, "Latin explanation");

  FlattenedCardText flattened;
  ASSERT_TRUE(flattenCardFieldsInPlace(fields, 2, storage.data(), storage.size(), flattened));
  constexpr char expected[] = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\n\nLatin explanation";
  ASSERT_STREQ(flattened.text, expected);

  CardTextLayout primaryLayout;
  EXPECT_FALSE(layoutCardTextWithPrimaryFields(
      flattened, /*left=*/0, /*top=*/0, /*right=*/200, /*bottom=*/30, /*primaryLineHeight=*/20,
      /*normalLineHeight=*/10, primaryLayout, [](const FontRole, const char* value) {
        return static_cast<int>(std::strlen(value));
      }));

  RecordingFontCache fontCache;
  std::vector<int> renderedFontIds;
  ASSERT_TRUE(renderCardTextWithPrimaryAndNormalPrewarm(
      fontCache, /*usePrimary=*/false, /*primaryFontId=*/43, /*normalFontId=*/42,
      [&fontCache, &renderedFontIds](const FontRole role, const int fontId) {
        fontCache.lastRole = role;
        fontCache.events.push_back(fontCache.scanning
                                       ? (role == FontRole::Primary ? RenderEvent::PrimaryScanDraw
                                                                    : RenderEvent::NormalScanDraw)
                                       : (role == FontRole::Primary ? RenderEvent::PrimaryRealDraw
                                                                    : RenderEvent::NormalRealDraw));
        renderedFontIds.push_back(fontId);
      }));

  const std::vector<RenderEvent> expectedEvents{RenderEvent::NormalScanDraw, RenderEvent::NormalEndScanAndPrewarm,
                                                 RenderEvent::NormalRealDraw};
  EXPECT_EQ(fontCache.events, expectedEvents);
  EXPECT_EQ(renderedFontIds, (std::vector<int>{42, 42}));
}

TEST(AnkiCardText, UsesPrimaryThenNormalPrewarmPassesOnlyForAFittingPrimaryAndSecondaryLayout) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  setField(fields, storage.data(), 0, "Term");
  fields[0].primary = true;
  setField(fields, storage.data(), 1, "short definition");

  FlattenedCardText flattened;
  ASSERT_TRUE(flattenCardFieldsInPlace(fields, 2, storage.data(), storage.size(), flattened));
  CardTextLayout layout;
  ASSERT_TRUE(layoutCardTextWithPrimaryFields(
      flattened, /*left=*/0, /*top=*/0, /*right=*/200, /*bottom=*/100, /*primaryLineHeight=*/20,
      /*normalLineHeight=*/10, layout, [](const FontRole role, const char* value) {
        return static_cast<int>(std::strlen(value)) * (role == FontRole::Primary ? 12 : 8);
      }));
  ASSERT_EQ(layout.lineCount, 3U);
  EXPECT_EQ(layout.lines[0].role, FontRole::Primary);
  EXPECT_EQ(layout.lines[1].role, FontRole::Normal);
  EXPECT_EQ(layout.lines[2].role, FontRole::Normal);

  RecordingFontCache fontCache;
  std::vector<int> renderedFontIds;
  ASSERT_TRUE(renderCardTextWithPrimaryAndNormalPrewarm(
      fontCache, /*usePrimary=*/true, /*primaryFontId=*/43, /*normalFontId=*/42,
      [&fontCache, &renderedFontIds](const FontRole role, const int fontId) {
        fontCache.lastRole = role;
        fontCache.events.push_back(fontCache.scanning
                                       ? (role == FontRole::Primary ? RenderEvent::PrimaryScanDraw
                                                                    : RenderEvent::NormalScanDraw)
                                       : (role == FontRole::Primary ? RenderEvent::PrimaryRealDraw
                                                                    : RenderEvent::NormalRealDraw));
        renderedFontIds.push_back(fontId);
      }));

  const std::vector<RenderEvent> expectedEvents{
      RenderEvent::PrimaryScanDraw, RenderEvent::PrimaryEndScanAndPrewarm, RenderEvent::PrimaryRealDraw,
      RenderEvent::NormalScanDraw, RenderEvent::NormalEndScanAndPrewarm, RenderEvent::NormalRealDraw};
  EXPECT_EQ(fontCache.events, expectedEvents);
  EXPECT_EQ(renderedFontIds, (std::vector<int>{43, 43, 42, 42}));
}
TEST(AnkiCardText, RendersMixedJapaneseAndLatinPrimaryAtLargerFontBeforeNormalSecondary) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  setField(fields, storage.data(), 0, "\xE7\x8C\xAB Latin term");
  fields[0].primary = true;
  setField(fields, storage.data(), 1, "\xE8\xAA\xAC\xE6\x98\x8E Latin detail");

  FlattenedCardText flattened;
  ASSERT_TRUE(flattenCardFieldsInPlace(fields, 2, storage.data(), storage.size(), flattened));
  constexpr char expected[] = "\xE7\x8C\xAB Latin term\n\n\xE8\xAA\xAC\xE6\x98\x8E Latin detail";
  ASSERT_STREQ(flattened.text, expected);

  CardTextLayout layout;
  ASSERT_TRUE(layoutCardTextWithPrimaryFields(
      flattened, /*left=*/0, /*top=*/0, /*right=*/200, /*bottom=*/64, /*primaryLineHeight=*/24,
      /*normalLineHeight=*/12, layout, [](const FontRole role, const char* value) {
        return static_cast<int>(std::strlen(value)) * (role == FontRole::Primary ? 9 : 5);
      }));
  ASSERT_EQ(layout.lineCount, 3U);
  EXPECT_EQ(layout.lines[0].role, FontRole::Primary);
  EXPECT_EQ(std::string(flattened.text + layout.lines[0].start, layout.lines[0].length), "\xE7\x8C\xAB Latin term");
  EXPECT_EQ(layout.lines[1].role, FontRole::Normal);
  EXPECT_EQ(layout.lines[2].role, FontRole::Normal);
  EXPECT_EQ(std::string(flattened.text + layout.lines[2].start, layout.lines[2].length),
            "\xE8\xAA\xAC\xE6\x98\x8E Latin detail");

  RecordingFontCache fontCache;
  std::vector<FontRole> renderedRoles;
  std::vector<int> renderedFontIds;
  ASSERT_TRUE(renderCardTextWithPrimaryAndNormalPrewarm(
      fontCache, /*usePrimary=*/true, /*primaryFontId=*/43, /*normalFontId=*/42,
      [&fontCache, &renderedRoles, &renderedFontIds](const FontRole role, const int fontId) {
        fontCache.lastRole = role;
        fontCache.events.push_back(fontCache.scanning
                                       ? (role == FontRole::Primary ? RenderEvent::PrimaryScanDraw
                                                                    : RenderEvent::NormalScanDraw)
                                       : (role == FontRole::Primary ? RenderEvent::PrimaryRealDraw
                                                                    : RenderEvent::NormalRealDraw));
        renderedRoles.push_back(role);
        renderedFontIds.push_back(fontId);
      }));

  const std::vector<RenderEvent> expectedEvents{
      RenderEvent::PrimaryScanDraw, RenderEvent::PrimaryEndScanAndPrewarm, RenderEvent::PrimaryRealDraw,
      RenderEvent::NormalScanDraw, RenderEvent::NormalEndScanAndPrewarm, RenderEvent::NormalRealDraw};
  EXPECT_EQ(fontCache.events, expectedEvents);
  EXPECT_EQ(renderedRoles,
            (std::vector<FontRole>{FontRole::Primary, FontRole::Primary, FontRole::Normal, FontRole::Normal}));
  EXPECT_EQ(renderedFontIds, (std::vector<int>{43, 43, 42, 42}));
}

TEST(AnkiCardText, CentersWrappedJapaneseAndLatinLinesUsingOneSelectedFontAdvance) {
  char text[] = "\xE7\x8C\xAB Latin";
  CardTextLayout layout;

  ASSERT_TRUE(layoutCardText(
      text, static_cast<uint16_t>(std::strlen(text)), /*left=*/10, /*top=*/20, /*right=*/110, /*bottom=*/100,
      /*lineHeight=*/10, layout, [](const char* value) {
        int glyphs = 0;
        for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(value); *cursor != '\0'; ++cursor) {
          if ((*cursor & 0xC0) != 0x80) ++glyphs;
        }
        return glyphs * 20;
      }));

  ASSERT_FALSE(layout.topLeftFallback);
  ASSERT_EQ(layout.lineCount, 2U);
  EXPECT_EQ(layout.lines[0].start, 0U);
  EXPECT_EQ(layout.lines[0].length, 3U);
  EXPECT_EQ(layout.lines[0].x, 50);
  EXPECT_EQ(layout.lines[0].y, 50);
  EXPECT_EQ(layout.lines[1].start, 4U);
  EXPECT_EQ(layout.lines[1].length, 5U);
  EXPECT_EQ(layout.lines[1].x, 10);
  EXPECT_EQ(layout.lines[1].y, 60);
}

TEST(AnkiCardText, FallsBackToSelectedFontTopLeftLayoutWhenWrappedBlockOverflows) {
  char text[] = "one\ntwo\nthree";
  CardTextLayout layout;

  ASSERT_TRUE(layoutCardText(
      text, static_cast<uint16_t>(std::strlen(text)), /*left=*/10, /*top=*/20, /*right=*/110, /*bottom=*/40,
      /*lineHeight=*/10, layout, [](const char* value) { return static_cast<int>(std::strlen(value)); }));

  EXPECT_TRUE(layout.topLeftFallback);
  EXPECT_EQ(layout.lineCount, 0U);
}

TEST(AnkiCardText, RejectsBlocksWhoseSeparatorsExceedSideLimit) {
  std::array<char, kFieldStorageBytes> storage{};
  std::array<CardField, kMaxCardFields> fields{};
  std::memset(storage.data(), 'a', kMaxCardFieldTextBytes);
  storage[kMaxCardFieldTextBytes] = '\0';
  std::memset(storage.data() + kMaxCardFieldTextBytes + 1, 'b', kMaxCardFieldTextBytes);
  storage[2 * (kMaxCardFieldTextBytes + 1) - 1] = '\0';
  fields[0] = {storage.data(), kMaxCardFieldTextBytes, true};
  fields[1] = {storage.data() + kMaxCardFieldTextBytes + 1, kMaxCardFieldTextBytes, false};

  FlattenedCardText flattened;
  EXPECT_FALSE(flattenCardFieldsInPlace(fields, 2, storage.data(), storage.size(), flattened));
  EXPECT_EQ(flattened.text, nullptr);
  EXPECT_EQ(flattened.length, 0);
}

}  // namespace
