#pragma once
#include <MangaFormat.h>

#include <cstdint>

struct MangaTranslationPageResult {
  bool more = false, empty = true, error = false;
};
// Allocation-free fixed-cell paging over immutable, length-delimited stored
// translations. Rewalking <=32 KiB makes Back bounded without a page-offset heap.
// Each callback receives one terminated UTF-8 glyph, valid only during the call.
inline MangaTranslationPageResult renderMangaTranslationPage(manga::format::PageView page, int scope,
                                                             uint32_t targetPage, int columns, int rows,
                                                             void (*draw)(void*, const char*, int, int),
                                                             void* context) {
  MangaTranslationPageResult result;
  if (scope < -1 || scope >= page.panels.remaining || columns <= 0 || rows <= 0) {
    result.error = true;
    return result;
  }
  uint32_t line = 0;
  int column = 0, index = 0;
  auto panels = page.panels;
  while (panels.remaining) {
    manga::format::PanelView panel;
    if (panels.next(panel) != manga::format::Error::None) {
      result.error = true;
      return result;
    }
    if (scope >= 0 && index++ != scope) continue;
    const auto text = panel.translation;
    for (size_t offset = 0; offset < text.size();) {
      const uint8_t lead = static_cast<uint8_t>(text[offset]);
      if (lead == '\n' || lead == '\r') {
        ++offset;
        if (lead == '\r' && offset < text.size() && text[offset] == '\n') ++offset;
        column = 0;
        ++line;
        continue;
      }
      char glyph[5]{};
      size_t count = lead < 0x80                    ? 1
                     : lead >= 0xc2 && lead <= 0xdf ? 2
                     : lead >= 0xe0 && lead <= 0xef ? 3
                     : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                    : 0;
      bool valid = count && count <= text.size() - offset;
      uint32_t cp = count == 1 ? lead : lead & (0x7f >> count);
      for (size_t i = 1; valid && i < count; ++i) {
        const uint8_t byte = static_cast<uint8_t>(text[offset + i]);
        valid = (byte & 0xc0) == 0x80;
        cp = (cp << 6) | (byte & 0x3f);
      }
      valid = valid && !(count == 3 && cp < 0x800) && !(count == 4 && cp < 0x10000) &&
              !(cp >= 0xd800 && cp <= 0xdfff) && cp <= 0x10ffff;
      if (!valid) {
        glyph[0] = '?';
        count = 1;
      } else if (cp < 0x20 || cp == 0x7f)
        glyph[0] = ' ';
      else
        for (size_t i = 0; i < count; ++i) glyph[i] = text[offset + i];
      offset += count;
      if (column == columns) {
        column = 0;
        ++line;
      }
      const uint32_t currentPage = line / static_cast<uint32_t>(rows);
      if (currentPage > targetPage) {
        result.more = true;
        return result;
      }
      if (currentPage == targetPage) {
        result.empty = false;
        if (draw) draw(context, glyph, column, static_cast<int>(line % rows));
      }
      ++column;
    }
    if (column) {
      column = 0;
      ++line;
    }
    if (scope >= 0) break;
  }
  return result;
}
