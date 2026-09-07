#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace manga::format {
using Bytes = std::span<const uint8_t>;
constexpr uint32_t kMaxPages = 10000;
constexpr uint32_t kMaxPageBytes = 32768;
constexpr uint32_t kMaxTocEntries = 1000;
constexpr size_t kIndexHeaderBytes = 8;
constexpr size_t kIndexRecordBytes = 12;

enum class Error { None, End, Truncated, UnsupportedVersion, InvalidCount, PageTooLarge, DataExtent };
const char* errorName(Error error);

// All views and cursors borrow bytes from the caller. Keep the entire backing buffer
// alive and immutable until every derived view/cursor is discarded. Text is raw UTF-8
// bytes, may contain NUL, and is NOT terminated. No parsing function allocates or logs;
// storage/UI callers must log returned failures with their file/page context.
struct Rect {
  uint16_t x = 0, y = 0, w = 0, h = 0;
};
struct IndexHeader {
  uint32_t pageCount = 0;
};
struct IndexRecord {
  uint32_t dataOffset = 0, dataLength = 0;
  uint16_t imageWidth = 0, imageHeight = 0;
};
struct TextView {
  Rect box;
  std::string_view text;
};
struct TextCursor {
  Bytes bytes;
  uint16_t remaining = 0;
  Error next(TextView& out);
};
struct PanelView {
  Rect box;
  std::string_view translation;
  TextCursor texts;
};
struct PanelCursor {
  Bytes bytes;
  uint16_t remaining = 0;
  Error next(PanelView& out);
};
struct PageView {
  PanelCursor panels;
};
struct MetaView {
  std::string_view title, author, language;
};
struct TocEntryView {
  uint32_t pageIndex = 0;
  std::string_view title;
};
struct TocCursor {
  Bytes bytes;
  uint32_t remaining = 0;
  Error next(TocEntryView& out);
};
struct TocView {
  TocCursor entries;
};

// Header/record functions permit on-demand index reads without buffering the index.
// decodeIndex validates ALL declared nonempty records against the panels.dat size.
// Empty records preserve their unused offset without checking the data extent.
// Arithmetic uses subtraction/64-bit extents, never overflowing offset + length.
Error decodeIndexHeader(Bytes bytes, IndexHeader& out);
Error decodeIndexRecord(Bytes bytes, uint64_t dataSize, IndexRecord& out);
Error decodeIndex(Bytes bytes, uint64_t dataSize, IndexHeader& out);
Error decodePage(Bytes bytes, PageView& out);
Error decodeMeta(Bytes bytes, MetaView& out);
Error decodeToc(Bytes bytes, TocView& out);
// Every failure clears out, including iterator End; cursors advance only on success.
// Complete page/TOC validation happens before publishing any view. Reserved bytes
// and trailing extension bytes are ignored. An empty page span is a valid empty page.
}  // namespace manga::format
