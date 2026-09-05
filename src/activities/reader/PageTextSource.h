#pragma once

#include <EpdFontFamily.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "util/DictionaryEngineTypes.h"

class GfxRenderer;
class Page;

struct PageTextGlyph {
  static constexpr uint16_t kSyntheticPageWord = UINT16_MAX;

  uint32_t codepoint = 0;
  uint16_t paragraph = 0;
  uint16_t pageWord = kSyntheticPageWord;
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

static_assert(sizeof(PageTextGlyph) == 16, "Page-text glyph metadata must stay compact on ESP32-C3");

struct PageTextSourceView {
  const PageTextGlyph* glyphs = nullptr;
  uint16_t glyphCount = 0;
  uint32_t contentHash = 0;
};

struct PageTextBounds {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

// Unions the rendered source-word rectangles for a scanner candidate. Synthetic
// separators have no rectangle and are ignored.
bool unionPageTextGlyphBounds(PageTextSourceView source, uint16_t firstGlyph, uint16_t glyphCount, PageTextBounds& out);

// Shared horizontal word geometry used both by the page-source adapter and by
// the temporary legacy word-selection activity. Text is borrowed from Page and
// remains valid only for the duration of the callback.
struct HorizontalPageWordGeometry {
  const char* text = nullptr;
  size_t textLength = 0;
  uint16_t sourceWordIndex = 0;
  uint16_t pageWord = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  uint8_t bionicBoundary = 0;
  uint16_t bionicRunOffset = 0;
  bool isRtl = false;
  bool selectable = false;
  bool containsDashSeparator = false;
  bool joinWithoutSpaceBefore = false;
  int16_t previousJoinedWidth = 0;
  bool endsWithInsertedHyphen = false;
};

struct HorizontalPageWordSink {
  void* context = nullptr;
  bool (*onWord)(void*, const HorizontalPageWordGeometry&) = nullptr;
};

// measurementScratch is optional. The legacy activity supplies its existing
// exact-size arena so soft-hyphen measurements preserve its prior behavior.
DictionaryStatus visitHorizontalPageWords(const Page& page, GfxRenderer& renderer, int fontId, int marginLeft,
                                          int marginTop, char* measurementScratch, size_t scratchCapacity,
                                          HorizontalPageWordSink sink);

int16_t measureHorizontalPageText(const GfxRenderer& renderer, int fontId, const char* text, size_t textLength,
                                  EpdFontFamily::Style style, char* scratch, size_t scratchCapacity);

class HorizontalPageTextSource {
 public:
  // Copies value metadata only and retains no reference to Page. The retry may
  // release an SD-font cache, so firmware callers must hold the activity's
  // RenderLock for the duration of build().
  DictionaryStatus build(const Page& page, GfxRenderer& renderer, int fontId, int marginLeft, int marginTop);
  // The returned glyph pointer remains valid only until the next build(),
  // clear(), or destruction of this source.
  PageTextSourceView view() const;
  bool truncated() const { return truncated_; }
  void clear();

 private:
  std::unique_ptr<PageTextGlyph[]> glyphs_;
  uint16_t glyphCount_ = 0;
  uint32_t contentHash_ = 0;
  bool truncated_ = false;
};
