#pragma once

#include "PageTextSource.h"

struct JapaneseLookupContext {
  char text[53]{};  // Three preceding and ten current/following UTF-8 characters.
  uint8_t byteCount = 0;
  uint8_t cursorByteOffset = 0;
};

inline bool buildJapaneseLookupContext(PageTextSourceView source, uint16_t firstGlyph, JapaneseLookupContext& out) {
  out = {};
  if (!source.glyphs || firstGlyph >= source.glyphCount) return false;
  const auto paragraph = source.glyphs[firstGlyph].paragraph;
  uint16_t start = firstGlyph;
  for (uint8_t back = 0; back < 3 && start > 0 && source.glyphs[start - 1].paragraph == paragraph; ++back) --start;
  for (uint32_t index = start; index < source.glyphCount && index < static_cast<uint32_t>(firstGlyph) + 10 &&
                               source.glyphs[index].paragraph == paragraph;
       ++index) {
    if (index == firstGlyph) out.cursorByteOffset = out.byteCount;
    const uint32_t cp = source.glyphs[index].codepoint;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      out = {};
      return false;
    }
    char* next = out.text + out.byteCount;
    if (cp < 0x80) {
      *next = static_cast<char>(cp);
      ++out.byteCount;
    } else if (cp < 0x800) {
      *next++ = static_cast<char>(0xC0 | (cp >> 6));
      *next = static_cast<char>(0x80 | (cp & 0x3F));
      out.byteCount += 2;
    } else if (cp < 0x10000) {
      *next++ = static_cast<char>(0xE0 | (cp >> 12));
      *next++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      *next = static_cast<char>(0x80 | (cp & 0x3F));
      out.byteCount += 3;
    } else {
      *next++ = static_cast<char>(0xF0 | (cp >> 18));
      *next++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      *next++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      *next = static_cast<char>(0x80 | (cp & 0x3F));
      out.byteCount += 4;
    }
  }
  return true;
}
