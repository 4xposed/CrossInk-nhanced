#include "MangaFormat.h"

namespace manga::format {
namespace {
// Byte-wise loads work on unaligned SD buffers and on either host endianness.
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
Rect rect(const uint8_t* p) { return {u16(p), u16(p + 2), u16(p + 4), u16(p + 6)}; }
bool takeText(Bytes& bytes, size_t length, std::string_view& out) {
  if (length > bytes.size()) return false;
  if (length == 0) {
    out = {};
  } else {
    out = {reinterpret_cast<const char*>(bytes.data()), length};
  }
  bytes = bytes.subspan(length);
  return true;
}
}  // namespace

const char* errorName(Error error) {
  switch (error) {
    case Error::None:
      return "success";
    case Error::End:
      return "end of records";
    case Error::Truncated:
      return "truncated record";
    case Error::UnsupportedVersion:
      return "unsupported format version";
    case Error::InvalidCount:
      return "invalid record count";
    case Error::PageTooLarge:
      return "page exceeds 32768 bytes";
    case Error::DataExtent:
      return "page record exceeds data extent";
  }
  return "unknown format error";
}

Error decodeIndexHeader(Bytes bytes, IndexHeader& out) {
  out = {};
  if (bytes.size() < kIndexHeaderBytes) return Error::Truncated;
  if (u32(bytes.data()) != 2) return Error::UnsupportedVersion;
  const uint32_t count = u32(bytes.data() + 4);
  if (count == 0 || count > kMaxPages) return Error::InvalidCount;
  out.pageCount = count;
  return Error::None;
}

Error decodeIndexRecord(Bytes bytes, uint64_t dataSize, IndexRecord& out) {
  out = {};
  if (bytes.size() < kIndexRecordBytes) return Error::Truncated;
  const IndexRecord record{u32(bytes.data()), u32(bytes.data() + 4), u16(bytes.data() + 8), u16(bytes.data() + 10)};
  if (record.dataLength > kMaxPageBytes) return Error::PageTooLarge;
  // Upstream empty pages never seek/read data, so their unused offset is irrelevant.
  if (record.dataLength != 0 && (record.dataOffset > dataSize || record.dataLength > dataSize - record.dataOffset))
    return Error::DataExtent;
  out = record;
  return Error::None;
}

Error decodeIndex(Bytes bytes, uint64_t dataSize, IndexHeader& out) {
  out = {};
  IndexHeader header;
  const Error error = decodeIndexHeader(bytes, header);
  if (error != Error::None) return error;
  bytes = bytes.subspan(kIndexHeaderBytes);
  if (header.pageCount > bytes.size() / kIndexRecordBytes) return Error::Truncated;
  for (uint32_t i = 0; i < header.pageCount; ++i) {
    IndexRecord record;
    const Error recordError = decodeIndexRecord(bytes, dataSize, record);
    if (recordError != Error::None) return recordError;
    bytes = bytes.subspan(kIndexRecordBytes);
  }
  out = header;
  return Error::None;
}

Error TextCursor::next(TextView& out) {
  out = {};
  if (remaining == 0) return Error::End;
  if (bytes.size() < 10) return Error::Truncated;
  TextView text;
  text.box = rect(bytes.data());
  const uint16_t length = u16(bytes.data() + 8);
  Bytes rest = bytes.subspan(10);
  if (!takeText(rest, length, text.text)) return Error::Truncated;
  out = text;
  bytes = rest;
  --remaining;
  return Error::None;
}

Error PanelCursor::next(PanelView& out) {
  out = {};
  if (remaining == 0) return Error::End;
  if (bytes.size() < 12) return Error::Truncated;
  PanelView panel;
  panel.box = rect(bytes.data());
  const uint8_t textCount = bytes[8];
  const uint16_t length = u16(bytes.data() + 10);
  Bytes rest = bytes.subspan(12);
  if (!takeText(rest, length, panel.translation)) return Error::Truncated;
  TextCursor cursor{rest, textCount};
  for (uint16_t i = 0; i < textCount; ++i) {
    TextView text;
    const Error error = cursor.next(text);
    if (error != Error::None) return error;
  }
  panel.texts = {rest.first(rest.size() - cursor.bytes.size()), textCount};
  out = panel;
  bytes = cursor.bytes;
  --remaining;
  return Error::None;
}

Error decodePage(Bytes bytes, PageView& out) {
  out = {};
  if (bytes.size() > kMaxPageBytes) return Error::PageTooLarge;
  if (bytes.empty()) return Error::None;
  if (bytes.size() < 2) return Error::Truncated;
  const uint8_t count = bytes[0];
  const Bytes records = bytes.subspan(2);
  PanelCursor cursor{records, count};
  for (uint16_t i = 0; i < count; ++i) {
    PanelView panel;
    const Error error = cursor.next(panel);
    if (error != Error::None) return error;
  }
  out.panels = {records.first(records.size() - cursor.bytes.size()), count};
  return Error::None;
}

Error decodeMeta(Bytes bytes, MetaView& out) {
  out = {};
  if (bytes.size() < 8) return Error::Truncated;
  if (u32(bytes.data()) != 1) return Error::UnsupportedVersion;
  const uint16_t titleLength = u16(bytes.data() + 4);
  const uint16_t authorLength = u16(bytes.data() + 6);
  bytes = bytes.subspan(8);
  MetaView meta;
  if (!takeText(bytes, titleLength, meta.title) || !takeText(bytes, authorLength, meta.author)) return Error::Truncated;
  if (!bytes.empty()) {
    if (bytes.size() < 2) return Error::Truncated;
    const uint16_t languageLength = u16(bytes.data());
    bytes = bytes.subspan(2);
    if (!takeText(bytes, languageLength, meta.language)) return Error::Truncated;
  }
  out = meta;
  return Error::None;
}

Error TocCursor::next(TocEntryView& out) {
  out = {};
  if (remaining == 0) return Error::End;
  if (bytes.size() < 6) return Error::Truncated;
  TocEntryView entry;
  entry.pageIndex = u32(bytes.data());
  const uint16_t length = u16(bytes.data() + 4);
  Bytes rest = bytes.subspan(6);
  if (!takeText(rest, length, entry.title)) return Error::Truncated;
  out = entry;
  bytes = rest;
  --remaining;
  return Error::None;
}

Error decodeToc(Bytes bytes, TocView& out) {
  out = {};
  if (bytes.size() < 8) return Error::Truncated;
  if (u32(bytes.data()) != 1) return Error::UnsupportedVersion;
  const uint32_t count = u32(bytes.data() + 4);
  if (count > kMaxTocEntries) return Error::InvalidCount;
  const Bytes records = bytes.subspan(8);
  TocCursor cursor{records, count};
  for (uint32_t i = 0; i < count; ++i) {
    TocEntryView entry;
    const Error error = cursor.next(entry);
    if (error != Error::None) return error;
  }
  out.entries = {records.first(records.size() - cursor.bytes.size()), count};
  return Error::None;
}
}  // namespace manga::format
