#include <gtest/gtest.h>

#include <climits>
#include <string>
#include <vector>

#include "util/DictLayout.h"

namespace {

int measure(void*, const char* text, EpdFontFamily::Style, bool) { return static_cast<int>(std::string(text).size()); }

struct CapturedLine {
  std::string text;
  std::vector<EpdFontFamily::Style> styles;
  std::vector<bool> ipa;
  uint8_t indentLevel = 0;
  bool listItem = false;
};

void capture(void* ctx, const DictLayout::LayoutLineView& line) {
  auto& lines = *static_cast<std::vector<CapturedLine>*>(ctx);
  CapturedLine captured;
  captured.indentLevel = line.indentLevel;
  captured.listItem = line.isListItem;
  for (uint16_t i = 0; i < line.segmentCount; ++i) {
    const auto& segment = line.segments[i];
    captured.text.append(line.textPool + segment.offset, segment.length);
    captured.styles.push_back(segment.style);
    captured.ipa.push_back(segment.isIpa);
  }
  lines.push_back(std::move(captured));
}

int measureView(void*, std::string_view text, EpdFontFamily::Style, bool) {
  int codepoints = 0;
  for (const unsigned char value : text) {
    if ((value & 0xC0U) != 0x80U) ++codepoints;
  }
  return codepoints;
}

int measureZero(void*, std::string_view, EpdFontFamily::Style, bool) { return 0; }

int measureDouble(void*, std::string_view text, EpdFontFamily::Style, bool) {
  int codepoints = 0;
  for (const unsigned char value : text) {
    if ((value & 0xC0U) != 0x80U) ++codepoints;
  }
  return codepoints * 2;
}

int measureFreshLineLimit(void*, std::string_view text, EpdFontFamily::Style, bool) {
  if (text == "b") return INT_MAX;
  if (text == " ") return 1;
  return 0;
}

bool captureControlled(void* context, const DictLayout::LayoutLineView& line) {
  capture(context, line);
  return true;
}

}  // namespace

TEST(DictLayout, ReusableLineViewPreservesWrapAndStyleRuns) {
  const std::vector<StyledSpan> spans = {
      {.text = "alpha beta", .bold = true},
      {.text = " gamma"},
  };
  std::vector<CapturedLine> lines;
  const DictLayout::Measurer measurer{nullptr, measure};
  const DictLayout::LineSink sink{&lines, capture};

  DictLayout::wrapSpans(spans, {.maxWidth = 10}, measurer, sink);

  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, "alpha beta");
  EXPECT_EQ(lines[1].text, "gamma");
  ASSERT_EQ(lines[0].styles.size(), 1u);
  EXPECT_EQ(lines[0].styles[0], EpdFontFamily::BOLD);
  ASSERT_EQ(lines[1].styles.size(), 1u);
  EXPECT_EQ(lines[1].styles[0], EpdFontFamily::REGULAR);
}

TEST(DictLayout, SameStyleSegmentsMergeInTheBorrowedView) {
  const std::vector<StyledSpan> spans = {{.text = "one"}, {.text = "two"}};
  std::vector<CapturedLine> lines;
  const DictLayout::Measurer measurer{nullptr, measure};
  const DictLayout::LineSink sink{&lines, capture};

  DictLayout::wrapSpans(spans, {.maxWidth = 20}, measurer, sink);

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "onetwo");
  EXPECT_EQ(lines[0].styles.size(), 1u);
}

TEST(DictLayout, OversizedTokenBreaksAtCharacterBoundaries) {
  const std::vector<StyledSpan> spans = {{.text = "abcdefghij"}};
  std::vector<CapturedLine> lines;
  const DictLayout::Measurer measurer{nullptr, measure};
  const DictLayout::LineSink sink{&lines, capture};

  DictLayout::wrapSpans(spans, {.maxWidth = 4}, measurer, sink);

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].text, "abcd");
  EXPECT_EQ(lines[1].text, "efgh");
  EXPECT_EQ(lines[2].text, "ij");
}

TEST(DictLayout, ExplicitLineBreakFlushesStreamedText) {
  std::vector<CapturedLine> lines;
  const DictLayout::Measurer measurer{nullptr, measure};
  const DictLayout::LineSink sink{&lines, capture};
  DictLayout::Wrapper wrapper({.maxWidth = 20}, measurer, sink);

  wrapper.onSpan({.text = "alpha"});
  wrapper.lineBreak();
  wrapper.onSpan({.text = "beta"});
  wrapper.finish();

  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, "alpha");
  EXPECT_EQ(lines[1].text, "beta");
}

TEST(DictLayout, LengthAwareWrapperCarriesWordsAndUtf8AcrossNonTerminatedChunks) {
  const std::string text = "ab 猫犬 tail";
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = 4}, {nullptr, measureView}, {&lines, captureControlled}, scratch);

  ASSERT_TRUE(wrapper.onSpan({std::string_view(text.data(), 4)}));      // ends after first byte of 猫
  ASSERT_TRUE(wrapper.onSpan({std::string_view(text.data() + 4, 3)}));  // ends after first byte of 犬
  ASSERT_TRUE(wrapper.onSpan({std::string_view(text.data() + 7, text.size() - 7)}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].text, "ab");
  EXPECT_EQ(lines[1].text, "猫犬");
  EXPECT_EQ(lines[2].text, "tail");
}

TEST(DictLayout, LengthAwareWrapperPreservesExplicitIpaAndAllStyleMetadata) {
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = 20}, {nullptr, measureView}, {&lines, captureControlled}, scratch);
  const auto styled =
      static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD_ITALIC | EpdFontFamily::UNDERLINE | EpdFontFamily::SUP);

  ASSERT_TRUE(wrapper.onSpan({"ascii", styled, true, true, true, 2}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "ascii");
  EXPECT_EQ(lines[0].styles, std::vector<EpdFontFamily::Style>{styled});
  EXPECT_EQ(lines[0].ipa, std::vector<bool>{true});
  EXPECT_EQ(lines[0].indentLevel, 2);
  EXPECT_TRUE(lines[0].listItem);
}

TEST(DictLayout, LengthAwareWrapperRejectsInvalidAndTrailingPartialUtf8) {
  {
    std::vector<CapturedLine> lines;
    DictLayout::BoundedWrapScratch scratch;
    DictLayout::BoundedWrapper wrapper({.maxWidth = 20}, {nullptr, measureView}, {&lines, captureControlled}, scratch);
    const char invalid[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    EXPECT_FALSE(wrapper.onSpan({std::string_view(invalid, sizeof(invalid))}));
    EXPECT_EQ(wrapper.status(), DictLayout::BoundedWrapStatus::InvalidUtf8);
  }
  {
    std::vector<CapturedLine> lines;
    DictLayout::BoundedWrapScratch scratch;
    DictLayout::BoundedWrapper wrapper({.maxWidth = 20}, {nullptr, measureView}, {&lines, captureControlled}, scratch);
    const char partial[] = {static_cast<char>(0xE7), static_cast<char>(0x8C)};
    ASSERT_TRUE(wrapper.onSpan({std::string_view(partial, sizeof(partial))}));
    EXPECT_FALSE(wrapper.finish());
    EXPECT_EQ(wrapper.status(), DictLayout::BoundedWrapStatus::InvalidUtf8);
  }
}

TEST(DictLayout, PendingSeparatorDoesNotCrossTheFixedLineByteCapacity) {
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = 1}, {nullptr, measureZero}, {&lines, captureControlled}, scratch);
  const std::string prefix(DictLayout::BoundedWrapScratch::kTextCapacity - 1, 'a');

  ASSERT_TRUE(wrapper.onSpan({prefix}));
  ASSERT_TRUE(wrapper.onSpan({" "}));
  ASSERT_TRUE(wrapper.onSpan({"b"}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, prefix);
  EXPECT_EQ(lines[1].text, "b");
}

TEST(DictLayout, PendingSeparatorDoesNotCrossTheFixedLineSegmentCapacity) {
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = 1}, {nullptr, measureZero}, {&lines, captureControlled}, scratch);
  for (uint16_t index = 0; index < DictLayout::BoundedWrapScratch::kSegmentCapacity; ++index) {
    const auto style = index % 2 == 0 ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
    ASSERT_TRUE(wrapper.onSpan({"x", style}));
  }

  ASSERT_TRUE(wrapper.onSpan({" "}));
  ASSERT_TRUE(wrapper.onSpan({"y"}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, std::string(DictLayout::BoundedWrapScratch::kSegmentCapacity, 'x'));
  EXPECT_EQ(lines[1].text, "y");
}

TEST(DictLayout, DroppedCapacitySeparatorDoesNotContributeToFreshLineOverflow) {
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = INT_MAX}, {nullptr, measureFreshLineLimit},
                                     {&lines, captureControlled}, scratch);
  const std::string prefix(DictLayout::BoundedWrapScratch::kTextCapacity - 1, 'a');

  ASSERT_TRUE(wrapper.onSpan({prefix}));
  ASSERT_TRUE(wrapper.onSpan({" "}));
  ASSERT_TRUE(wrapper.onSpan({"b"}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, prefix);
  EXPECT_EQ(lines[1].text, "b");
}

TEST(DictLayout, SuperscriptSeparatorUsesTheRenderedHalfAdvanceAtWrapBoundary) {
  std::vector<CapturedLine> lines;
  DictLayout::BoundedWrapScratch scratch;
  DictLayout::BoundedWrapper wrapper({.maxWidth = 4}, {nullptr, measureDouble}, {&lines, captureControlled}, scratch);

  ASSERT_TRUE(wrapper.onSpan({"a"}));
  ASSERT_TRUE(wrapper.onSpan({" b", EpdFontFamily::SUP}));
  ASSERT_TRUE(wrapper.finish());

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "a b");
}
