#pragma once

#include <MangaFormat.h>
#include <MangaImageGeometry.h>

#include "PageTextSource.h"

struct MangaLookupGeometry {
  int sourceWidth = 0, sourceHeight = 0;
  // Optional page-coordinate crop corresponding to the displayed panel image.
  manga::format::Rect sourceCrop{};
  manga::ImageViewports views;
  manga::ImageLayout layout;
  bool textOnly = false;
  bool approximateTextPositions = false;
  int cellWidth = 16, lineHeight = 24;
};
constexpr uint16_t kMangaLookupMaxGlyphs = 1024;  // 16 KiB glyphs + <=8 KiB scanner candidates.

bool mapMangaLookupBlock(manga::format::Rect box, const MangaLookupGeometry& geometry, PageTextBounds& out);
// Borrowed measurement context is used only during source construction.
struct MangaTextMeasure {
  void* context = nullptr;
  int (*advance)(void*, uint32_t) = nullptr;
};
DictionaryStatus buildMangaLookupTextSource(manga::format::PageView page, int panel,
                                            const MangaLookupGeometry& geometry, OwnedLookupTextSource& out,
                                            int region = -1, MangaTextMeasure measure = {});
bool mangaLookupRegionBounds(manga::format::PageView page, int panel, const MangaLookupGeometry& geometry, int region,
                             PageTextBounds& out);
int nextMangaLookupRegion(manga::format::PageView page, int panel, const MangaLookupGeometry& geometry, int current,
                          bool forward);
int mangaLookupRegionAtPoint(manga::format::PageView page, int panel, const MangaLookupGeometry& geometry, int x,
                             int y);
struct MangaLookupClippingRange {
  uint16_t firstPageWordOrdinal = 0, lastPageWordOrdinal = 0;
  uint16_t firstWordByteOffset = 0, lastWordByteEndOffset = 0;
};

bool copyMangaLookupClipping(manga::format::PageView page, int panel, MangaLookupClippingRange range, char* out,
                             size_t capacity, size_t& written);
bool mangaLookupRegionHasSingleToken(manga::format::PageView page, int panel, int region);
bool mangaLookupCacheFileName(uint32_t physicalPage, int panel, char* out, size_t capacity);
