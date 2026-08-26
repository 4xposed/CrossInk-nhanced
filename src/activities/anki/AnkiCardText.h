#pragma once

#include <AnkiDeckTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>

constexpr size_t kMaxCardTextLayoutLines = 256;

enum class FontRole : uint8_t {
  Primary,
  Normal,
};

struct CardTextLine {
  uint16_t start = 0;
  uint16_t length = 0;
  int16_t x = 0;
  int16_t y = 0;
  FontRole role = FontRole::Normal;

};

// Layout is bounded by what the screen can display. Excess wrapped lines use
// the established selected-font top/left fallback instead of allocating.
struct CardTextLayout {
  std::array<CardTextLine, kMaxCardTextLayoutLines> lines{};
  uint16_t lineCount = 0;
  bool topLeftFallback = false;
};

namespace anki_card_text_detail {
inline bool isUtf8Continuation(const unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

inline char* nextUtf8CodePoint(char* const cursor, char* const end) {
  char* next = cursor + 1;
  while (next < end && isUtf8Continuation(static_cast<unsigned char>(*next))) ++next;
  return next;
}

inline bool isWrapWhitespace(const char value) { return value == ' ' || value == '\t'; }
}  // namespace anki_card_text_detail

// Measures and stores every wrapped line at a card transition. Measure must
// accept a temporarily NUL-terminated line and return its selected-font advance.
template <typename Measure>
bool layoutCardText(char* const text, const uint16_t textLength, const int left, const int top, const int right,
                    const int bottom, const int lineHeight, CardTextLayout& out, Measure&& measure) {
  out = {};
  if (text == nullptr || textLength == 0 || right <= left || bottom <= top || lineHeight <= 0) return false;

  const int contentHeight = bottom - top;
  const int contentWidth = right - left;
  char* const textEnd = text + textLength;
  const auto measureRange = [&measure](char* const start, char* const end) {
    const char saved = *end;
    *end = '\0';
    const int advance = measure(start);
    *end = saved;
    return advance;
  };
  const auto fallback = [&out] {
    out = {};
    out.topLeftFallback = true;
    return true;
  };
  const auto appendLine = [&out, text, &measureRange](char* const start, char* const end) {
    if (out.lineCount == kMaxCardTextLayoutLines) return false;
    CardTextLine& line = out.lines[out.lineCount++];
    line.start = static_cast<uint16_t>(start - text);
    line.length = static_cast<uint16_t>(end - start);
    // The initial value is replaced with the centered x coordinate after every
    // line has been measured and the block height is known.
    line.x = static_cast<int16_t>(measureRange(start, end));
    return true;
  };

  char* paragraphStart = text;
  while (paragraphStart <= textEnd) {
    char* paragraphEnd = paragraphStart;
    while (paragraphEnd < textEnd && *paragraphEnd != '\n') ++paragraphEnd;
    if (paragraphStart == paragraphEnd) {
      if (!appendLine(paragraphStart, paragraphEnd)) return fallback();
    } else {
      char* lineStart = paragraphStart;
      while (lineStart < paragraphEnd) {
        char* cursor = lineStart;
        char* lastBreak = nullptr;
        bool emittedWrappedLine = false;
        while (cursor < paragraphEnd) {
          char* const next = anki_card_text_detail::nextUtf8CodePoint(cursor, paragraphEnd);
          if (measureRange(lineStart, next) <= contentWidth) {
            if (anki_card_text_detail::isWrapWhitespace(*cursor)) lastBreak = cursor;
            cursor = next;
            continue;
          }

          if (cursor == lineStart) cursor = next;
          if (lastBreak != nullptr && lastBreak > lineStart) {
            if (!appendLine(lineStart, lastBreak)) return fallback();
            lineStart = lastBreak + 1;
            while (lineStart < paragraphEnd && anki_card_text_detail::isWrapWhitespace(*lineStart)) ++lineStart;
          } else {
            if (!appendLine(lineStart, cursor)) return fallback();
            lineStart = cursor;
          }
          emittedWrappedLine = true;
          break;
        }
        if (!emittedWrappedLine) {
          if (!appendLine(lineStart, paragraphEnd)) return fallback();
          break;
        }
      }
    }
    if (paragraphEnd == textEnd) break;
    paragraphStart = paragraphEnd + 1;
  }

  const int totalHeight = static_cast<int>(out.lineCount) * lineHeight;
  if (totalHeight > contentHeight) return fallback();

  const int firstY = top + (contentHeight - totalHeight) / 2;
  for (uint16_t index = 0; index < out.lineCount; ++index) {
    CardTextLine& line = out.lines[index];
    char* const lineStart = text + line.start;
    char* const lineEnd = lineStart + line.length;
    const int advance = measureRange(lineStart, lineEnd);
    line.x = static_cast<int16_t>(left + (contentWidth - advance) / 2);
    line.y = static_cast<int16_t>(firstY + static_cast<int>(index) * lineHeight);
  }
  return true;
}

struct FlattenedCardText {
  char* text = nullptr;
  uint16_t length = 0;
  std::array<uint16_t, kMaxCardFields> fieldStarts{};
  std::array<uint16_t, kMaxCardFields> fieldLengths{};
  std::array<bool, kMaxCardFields> fieldPrimary{};
  uint8_t fieldCount = 0;
};

// Flattens ordered v2 field blocks into the allocation tail. The output is
// bounded by the format's 4,096-byte side limit and stays valid until the
// next field decode overwrites its backing buffer.
bool flattenCardFieldsInPlace(const std::array<CardField, kMaxCardFields>& fields, uint8_t fieldCount,
                              char* buffer, size_t bufferCapacity, FlattenedCardText& out);

// Lays out primary blocks at the supplied larger font metrics and secondary
// blocks at normal metrics. Unlike the baseline layout, inability to fit is a
// hard failure so the caller can retain the complete selected-font fallback.
template <typename Measure>
bool layoutCardTextWithPrimaryFields(const FlattenedCardText& text, const int left, const int top, const int right,
                                     const int bottom, const int primaryLineHeight, const int normalLineHeight,
                                     CardTextLayout& out, Measure&& measure) {
  out = {};
  if (text.text == nullptr || text.length == 0 || text.fieldCount == 0 || text.fieldCount > kMaxCardFields ||
      right <= left || bottom <= top || primaryLineHeight <= 0 || normalLineHeight <= 0) {
    return false;
  }

  const int contentWidth = right - left;
  const auto measureRange = [&measure](const FontRole role, char* const start, char* const end) {
    const char saved = *end;
    *end = '\0';
    const int advance = measure(role, start);
    *end = saved;
    return advance;
  };
  const auto appendLine = [&out, &text, &measureRange](const FontRole role, char* const start, char* const end) {
    if (out.lineCount == kMaxCardTextLayoutLines) return false;
    CardTextLine& line = out.lines[out.lineCount++];
    line.start = static_cast<uint16_t>(start - text.text);
    line.length = static_cast<uint16_t>(end - start);
    line.role = role;
    line.x = static_cast<int16_t>(measureRange(role, start, end));
    return true;
  };
  const auto appendParagraph = [&contentWidth, &measureRange, &appendLine](const FontRole role, char* lineStart,
                                                                            char* const paragraphEnd) {
    if (lineStart == paragraphEnd) return appendLine(role, lineStart, paragraphEnd);
    while (lineStart < paragraphEnd) {
      char* cursor = lineStart;
      char* lastBreak = nullptr;
      while (cursor < paragraphEnd) {
        char* const next = anki_card_text_detail::nextUtf8CodePoint(cursor, paragraphEnd);
        if (measureRange(role, lineStart, next) <= contentWidth) {
          if (anki_card_text_detail::isWrapWhitespace(*cursor)) lastBreak = cursor;
          cursor = next;
          continue;
        }
        if (cursor == lineStart) cursor = next;
        if (lastBreak != nullptr && lastBreak > lineStart) {
          if (!appendLine(role, lineStart, lastBreak)) return false;
          lineStart = lastBreak + 1;
          while (lineStart < paragraphEnd && anki_card_text_detail::isWrapWhitespace(*lineStart)) ++lineStart;
        } else {
          if (!appendLine(role, lineStart, cursor)) return false;
          lineStart = cursor;
        }
        break;
      }
      if (cursor == paragraphEnd) return appendLine(role, lineStart, paragraphEnd);
    }
    return true;
  };

  for (uint8_t index = 0; index < text.fieldCount; ++index) {
    const uint16_t startOffset = text.fieldStarts[index];
    const uint16_t length = text.fieldLengths[index];
    if (length == 0 || startOffset > text.length || length > text.length - startOffset) return false;
    char* paragraphStart = text.text + startOffset;
    char* const fieldEnd = paragraphStart + length;
    const FontRole role = text.fieldPrimary[index] ? FontRole::Primary : FontRole::Normal;
    while (paragraphStart <= fieldEnd) {
      char* paragraphEnd = paragraphStart;
      while (paragraphEnd < fieldEnd && *paragraphEnd != '\n') ++paragraphEnd;
      if (!appendParagraph(role, paragraphStart, paragraphEnd)) return false;
      if (paragraphEnd == fieldEnd) break;
      paragraphStart = paragraphEnd + 1;
    }
    if (index + 1 < text.fieldCount && !appendLine(FontRole::Normal, fieldEnd, fieldEnd)) return false;
  }

  int totalHeight = 0;
  for (uint16_t index = 0; index < out.lineCount; ++index) {
    totalHeight += out.lines[index].role == FontRole::Primary ? primaryLineHeight : normalLineHeight;
  }
  if (totalHeight > bottom - top) {
    out = {};
    return false;
  }

  int y = top + (bottom - top - totalHeight) / 2;
  for (uint16_t index = 0; index < out.lineCount; ++index) {
    CardTextLine& line = out.lines[index];
    char* const lineStart = text.text + line.start;
    char* const lineEnd = lineStart + line.length;
    const int advance = measureRange(line.role, lineStart, lineEnd);
    line.x = static_cast<int16_t>(left + (contentWidth - advance) / 2);
    line.y = static_cast<int16_t>(y);
    y += line.role == FontRole::Primary ? primaryLineHeight : normalLineHeight;
  }
  return true;
}

// Runs a render callback once while its glyphs are scanned, prewarms those
// glyphs, then renders the same flattened UTF-8 text for display.
template <typename FontCache, typename Render>
bool renderCardTextWithPrewarm(FontCache& fontCache, char* const text, const int fontId, Render&& render) {
  auto scope = fontCache.createPrewarmScope();
  render(text, fontId);
  if (!scope.endScanAndPrewarm()) return false;
  render(text, fontId);
  return true;
}

// Render each role in a complete scan → prewarm → real-draw pass. A failed
// primary layout uses only the normal pass, preserving the baseline sequence.
template <typename FontCache, typename Render>
bool renderCardTextWithPrimaryAndNormalPrewarm(FontCache& fontCache, const bool usePrimary, const int primaryFontId,
                                               const int normalFontId, Render&& render) {
  if (usePrimary) {
    auto primaryScope = fontCache.createPrewarmScope();
    render(FontRole::Primary, primaryFontId);
    if (!primaryScope.endScanAndPrewarm()) return false;
    render(FontRole::Primary, primaryFontId);
  }
  auto normalScope = fontCache.createPrewarmScope();
  render(FontRole::Normal, normalFontId);
  if (!normalScope.endScanAndPrewarm()) return false;
  render(FontRole::Normal, normalFontId);
  return true;
}
