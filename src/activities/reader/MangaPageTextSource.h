#pragma once

#include <MangaFormat.h>
#include <MangaImageGeometry.h>

#include "PageTextSource.h"

// Source extent is metadata's FULL page extent. layout must describe the full
// page background, never a legacy crop. With no overview, use textOnly instead.
struct MangaLookupGeometry {
  int sourceWidth = 0, sourceHeight = 0;
  manga::ImageViewports views;
  manga::ImageLayout layout;
  bool textOnly = false;
  // Text fallback is a fixed-cell contract: render each nonsynthetic codepoint
  // into its glyph rectangle (no image boxes). Wrap at cell boundaries.
  int cellWidth = 16, lineHeight = 24;
};
constexpr uint16_t kMangaLookupMaxGlyphs = 1024;  // 16 KiB glyphs + <=8 KiB scanner candidates.

bool mapMangaLookupBlock(manga::format::Rect box, const MangaLookupGeometry& geometry, PageTextBounds& out);
// panel=-1 means all panels in stored order; otherwise zero-based selected panel.
// Found may be truncated: UI MUST mark it and MUST NOT load/save a scan cache.
// No partial Latin token is published; Japanese runs retain a codepoint prefix.
// Caller logs context for invalid page views.
DictionaryStatus buildMangaLookupTextSource(manga::format::PageView page, int panel,
                                            const MangaLookupGeometry& geometry, OwnedLookupTextSource& out);
struct MangaLookupClippingRange {
  uint16_t firstPageWordOrdinal = 0, lastPageWordOrdinal = 0;
  uint16_t firstWordByteOffset = 0, lastWordByteEndOffset = 0;
};
// Rewalk the same immutable borrowed page and scope after child teardown. Exact
// original bytes are copied; ranges crossing blocks/control/malformed boundaries
// are rejected. Output is NUL terminated; failure clears length and output.
bool copyMangaLookupClipping(manga::format::PageView page, int panel, MangaLookupClippingRange range, char* out,
                             size_t capacity, size_t& written);
// Leaf name inside this book's cache directory. Separate decimal fields avoid
// page/panel packing collisions; -1 overview cannot collide with panel zero.
bool mangaLookupCacheFileName(uint32_t physicalPage, int panel, char* out, size_t capacity);
