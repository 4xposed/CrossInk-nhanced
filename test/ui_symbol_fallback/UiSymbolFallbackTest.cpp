#include <EpdFontFamily.h>
#include <Utf8.h>
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/ui_symbols_10.h>
#include <gtest/gtest.h>

namespace {
constexpr uint32_t POWER = 0x23FB;
const EpdFont symbols(&ui_symbols_10);
const EpdFont smallRegular(&inter_10_regular), smallBold(&inter_10_bold);
const EpdFont largeRegular(&inter_12_regular), largeBold(&inter_12_bold);
const EpdFontFamily small(&smallRegular, &smallBold, nullptr, nullptr, &symbols);
const EpdFontFamily large(&largeRegular, &largeBold, nullptr, nullptr, &symbols);
}  // namespace

TEST(UiSymbolFallback, ContainsExactlyOneGlyph) {
  EXPECT_EQ(sizeof(ui_symbols_10Glyphs) / sizeof(ui_symbols_10Glyphs[0]), 1u);
  EXPECT_EQ(sizeof(ui_symbols_10Intervals) / sizeof(ui_symbols_10Intervals[0]), 1u);
  EXPECT_EQ(ui_symbols_10Intervals[0].first, POWER);
  EXPECT_TRUE(symbols.hasCodepoint(POWER));
  EXPECT_FALSE(symbols.hasCodepoint('A'));
}

TEST(UiSymbolFallback, SharesTheSameRasterAtBothScalesAndStyles) {
  for (const auto* family : {&small, &large}) {
    for (const auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
      const auto glyph = family->getGlyphData(POWER, style);
      EXPECT_EQ(glyph.fontData, &ui_symbols_10);
      EXPECT_EQ(glyph.glyph, symbols.findGlyph(POWER));
      EXPECT_TRUE(family->hasCodepoint(POWER, style));
      EXPECT_EQ(family->getFallbackCodepoint(POWER, style), POWER);
      int width = 0, height = 0;
      family->getTextDimensions("⏻", &width, &height, style);
      EXPECT_EQ(width, 18);
      EXPECT_EQ(height, 18);
    }
  }
}

TEST(UiSymbolFallback, PreservesNormalGlyphsAndMissingGlyphBehavior) {
  EXPECT_EQ(small.getGlyphData('A').fontData, &inter_10_regular);
  EXPECT_EQ(large.getGlyphData('*', EpdFontFamily::BOLD).fontData, &inter_12_bold);
  const EpdFontFamily noFallback(&smallRegular);
  EXPECT_FALSE(noFallback.hasCodepoint(POWER));
  EXPECT_EQ(noFallback.getGlyphData(POWER).glyph, smallRegular.getGlyph(REPLACEMENT_GLYPH));
}

#include <builtinFonts/notosansjp_joyo_12_regular.h>

TEST(UiSymbolFallback, JapaneseFallbackKeepsGlyphOwnerAndPrimaryCoverageSeparate) {
  const EpdFont japanese(&notosansjp_joyo_12_regular);
  const EpdFontFamily mixed(&smallRegular, &smallBold, nullptr, nullptr, &symbols, &japanese);
  EXPECT_EQ(mixed.getGlyphData('A').fontData, &inter_10_regular);
  EXPECT_EQ(mixed.getGlyphData(POWER).fontData, &ui_symbols_10);
  EXPECT_EQ(mixed.getGlyphData(0x732B).fontData, &notosansjp_joyo_12_regular);
  EXPECT_EQ(mixed.getGlyphData(0x732B).glyph, japanese.findGlyph(0x732B));
  EXPECT_FALSE(mixed.hasCodepoint(0x732B));
  EpdFontFamily::setBuiltinLastResort(&japanese);
  EXPECT_EQ(small.getGlyphData(0x732B).fontData, &notosansjp_joyo_12_regular);
  EpdFontFamily::setBuiltinLastResort(nullptr);
}

TEST(UiSymbolFallback, SdLastResortCanBeDetachedBeforeFontDestruction) {
  const EpdFont japanese(&notosansjp_joyo_12_regular);
  EpdFontFamily::setSdLastResort(&japanese);
  EXPECT_EQ(small.getGlyphData(0x732B).fontData, &notosansjp_joyo_12_regular);
  EpdFontFamily::setSdLastResort(nullptr);
  EXPECT_FALSE(small.findGlyphData(0x732B).glyph);
}
