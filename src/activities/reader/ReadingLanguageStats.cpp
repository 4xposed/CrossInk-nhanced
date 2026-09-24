#include "ReadingLanguageStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kDaySeconds = 86400;
constexpr uint32_t kLastDay = 36524;  // 2099-12-31, same epoch/range as ReadingStatsUtils.
constexpr uint16_t kMaxRows = 730;
uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
bool alpha(char c) { return c >= 'a' && c <= 'z'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool same(const char* a, const char* b) { return memcmp(a, b, 4) == 0; }
bool readExact(HalFile& f, void* p, size_t n) { return f.read(p, n) == static_cast<int>(n); }
bool writeExact(HalFile& f, const void* p, size_t n) { return f.write(p, n) == n; }
uint16_t summarySize(uint8_t version, bool book) {
  static constexpr uint16_t books[] = {0, 11, 12, 16, 69, 73, 137};
  static constexpr uint16_t globals[] = {0, 13, 17, 159, 223};
  if (book) return version < 7 ? books[version] : 0;
  return version < 5 ? globals[version] : 0;
}
struct DayRow {
  uint32_t day = 0;
  uint32_t seconds[8]{};
};
static_assert(sizeof(DayRow) == 36);
bool readRow(HalFile& f, DayRow& row) {
  uint8_t field[4];
  if (!readExact(f, field, 4)) return false;
  row.day = le32(field);
  for (auto& value : row.seconds) {
    if (!readExact(f, field, 4)) return false;
    value = le32(field);
  }
  return true;
}
bool writeRow(HalFile& f, const DayRow& row) {
  uint8_t field[4];
  put32(field, row.day);
  if (!writeExact(f, field, 4)) return false;
  for (auto value : row.seconds) {
    put32(field, value);
    if (!writeExact(f, field, 4)) return false;
  }
  return true;
}
bool writeHeader(HalFile& f, uint16_t rows, uint32_t anchor) {
  uint8_t bytes[12] = {'L', 'D', 'A', 'Y', 1, 0, static_cast<uint8_t>(rows), static_cast<uint8_t>(rows >> 8)};
  put32(bytes + 8, anchor);
  return writeExact(f, bytes, sizeof(bytes));
}
uint32_t windowStart(uint32_t anchor) { return anchor > 729 ? anchor - 729 : 1; }
struct SpanDays {
  uint64_t start = 0, end = 0;
  uint32_t first = 0, last = 0;
  explicit SpanDays(const ReadingLanguageSpan* span) {
    if (!span || !span->seconds || !span->localStart.isValid() || span->localStart.hour > 23 ||
        span->localStart.minute > 59 || span->localStart.second > 59)
      return;
    const auto& dt = span->localStart;
    start =
        uint64_t(readingStatsDayIndex(dt.date)) * kDaySeconds + uint32_t(dt.hour) * 3600 + dt.minute * 60 + dt.second;
    end = std::min(start + span->seconds, uint64_t(kLastDay + 1) * kDaySeconds);
    first = std::max<uint32_t>(uint32_t(start / kDaySeconds), 1);
    last = static_cast<uint32_t>((end - 1) / kDaySeconds);
  }
  uint32_t seconds(uint32_t day) const {
    const uint64_t lo = std::max(start, uint64_t(day) * kDaySeconds);
    const uint64_t hi = std::min(end, uint64_t(day + 1) * kDaySeconds);
    return hi > lo ? static_cast<uint32_t>(hi - lo) : 0;
  }
};

// One input/output row; no retained history array. Iteration is bounded by 730 days.
__attribute__((noinline)) bool streamDays(HalFile* old, HalFile& out, const ReadingLanguageFileInfo& info,
                                          const ReadingLanguageTotals& totals, const ReadingLanguageSpan* span) {
  SpanDays added(span);
  const uint32_t anchor = std::max(info.anchor, added.last);
  const uint32_t first = windowStart(anchor);
  uint8_t slot = 1;
  if (span)
    for (uint8_t i = 0; i < 8; ++i)
      if (same(totals.entries[i].tag, span->normalizedTag)) {
        slot = i;
        break;
      }
  if (!writeHeader(out, 0, anchor)) return false;
  DayRow input, output;
  uint16_t remaining = info.rows, count = 0;
  if (old && info.hasAppendix && !old->seekSet(info.summarySize + 12)) return false;
  if (remaining && (!old || !readRow(*old, input))) return false;
  for (uint32_t day = first; day <= anchor && anchor; ++day) {
    while (remaining && input.day < day) {
      --remaining;
      if (remaining && !readRow(*old, input)) return false;
    }
    output = {};
    output.day = day;
    bool present = remaining && input.day == day;
    if (present) {
      output = input;
      --remaining;
      if (remaining && !readRow(*old, input)) return false;
    }
    const uint32_t extra = added.seconds(day);
    if (extra) {
      output.seconds[slot] = saturateReadingLanguageSeconds(output.seconds[slot], extra);
      present = true;
    }
    if (present) {
      if (!writeRow(out, output)) return false;
      ++count;
    }
  }
  const size_t end = out.position();
  return out.seekSet(info.summarySize) && writeHeader(out, count, anchor) && out.seekSet(end);
}

__attribute__((noinline)) bool unchangedSlots(HalFile& old, uint16_t summary, const ReadingLanguageTotals& totals) {
  ReadingLanguageTotals previous;
  if (!old.seekSet(summary - 64) || !readReadingLanguageTotals(old, previous)) return false;
  for (unsigned i = 0; i < 8; ++i)
    if (previous.entries[i].tag[0] && !same(previous.entries[i].tag, totals.entries[i].tag)) return false;
  return true;
}
__attribute__((noinline)) bool copyAppendix(HalFile& old, HalFile& out, const ReadingLanguageFileInfo& info) {
  if (!old.seekSet(info.summarySize)) return false;
  uint8_t chunk[64];
  size_t left = info.size - info.summarySize;
  while (left) {
    const size_t n = std::min(left, sizeof(chunk));
    if (!readExact(old, chunk, n) || !writeExact(out, chunk, n)) return false;
    left -= n;
  }
  return true;
}
__attribute__((noinline)) bool writeTemp(const char* tmp, const char* oldPath, bool book,
                                         const ReadingLanguageTotals& totals, const ReadingLanguageSpan* span,
                                         ReadingStatsPrefixWriter writer, const void* stats, size_t& size) {
  HalFile old;
  ReadingLanguageFileInfo info;
  info.summarySize = book ? 137 : 223;
  if (oldPath) {
    if (!Storage.openFileForRead("LSTATS", oldPath, old)) return false;
    if (!inspectReadingLanguageFile(old, book, ReadingStatsParseMode::Local, info)) {
      old.close();
      return false;
    }
    // A save must never change the identities used by previously persisted rows.
    if (info.version == (book ? 6 : 4)) {
      if (!unchangedSlots(old, info.summarySize, totals)) {
        LOG_ERR("LSTATS", "Refusing to relabel persisted language slots");
        old.close();
        return false;
      }
    }
  }
  HalFile out;
  if (!Storage.openFileForWrite("LSTATS", tmp, out)) {
    if (old) old.close();
    return false;
  }
  bool ok = writer(out, stats) && writeReadingLanguageTotals(out, totals);
  // Legacy prefixes have no appendix. New output always uses the new summary offset.
  if (ok && !span && info.hasAppendix) {
    ok = copyAppendix(old, out, info);
  } else if (ok) {
    // Stream reads the old offset, but patches the new output offset. Legacy has no rows.
    info.summarySize = book ? 137 : 223;
    ok = streamDays(old ? &old : nullptr, out, info, totals, span);
  }
  size = out.position();
  if (old && !old.close()) ok = false;
  if (ok) {
    out.flush();
    ok = out.sync();
  }
  if (!out.close()) ok = false;
  return ok;
}
// Keep validation/publication frames separate from the streamed writer on C3.
__attribute__((noinline)) bool verifyTemp(const char* tmp, bool book, size_t size) {
  HalFile check;
  ReadingLanguageFileInfo info;
  if (!Storage.openFileForRead("LSTATS", tmp, check)) return false;
  bool ok = check.fileSize64() == size && inspectReadingLanguageFile(check, book, ReadingStatsParseMode::Local, info);
  if (!check.close()) ok = false;
  return ok;
}
__attribute__((noinline)) bool promoteTemp(const char* tmp, const char* path, const char* backupPath,
                                           const char* oldPath, bool keepBackup) {
  // Reset/recovery keeps the validated backup and temporarily protects the primary separately.
  char recovery[96];
  int length = snprintf(recovery, sizeof(recovery), "%s.recovery", path);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(recovery)) {
    Storage.remove(tmp);
    return false;
  }
  const bool recoveryIsSource = oldPath && strcmp(oldPath, recovery) == 0;
  if (keepBackup)
    length = snprintf(recovery, sizeof(recovery), "%s%s", path, recoveryIsSource ? ".invalid" : ".recovery");
  else
    length = backupPath ? snprintf(recovery, sizeof(recovery), "%s", backupPath)
                        : snprintf(recovery, sizeof(recovery), "%s.bak", path);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(recovery)) {
    Storage.remove(tmp);
    return false;
  }
  bool rotated = false;
  if (Storage.exists(path)) {
    if (Storage.exists(recovery) && !Storage.remove(recovery)) {
      Storage.remove(tmp);
      return false;
    }
    if (!Storage.rename(path, recovery)) {
      Storage.remove(tmp);
      return false;
    }
    rotated = true;
  }
  if (!Storage.rename(tmp, path)) {
    LOG_ERR("LSTATS", "Could not publish stats: %s", path);
    if (rotated && !Storage.rename(recovery, path)) LOG_ERR("LSTATS", "Stats recovery retained: %s", recovery);
    Storage.remove(tmp);
    return false;
  }
  if (keepBackup && rotated && !Storage.remove(recovery)) LOG_ERR("LSTATS", "Published stats; recovery cleanup failed");
  return true;
}
}  // namespace

uint32_t saturateReadingLanguageSeconds(uint32_t a, uint32_t b) { return UINT32_MAX - a < b ? UINT32_MAX : a + b; }
bool normalizeReadingLanguage(std::string_view input, char (&out)[4]) {
  memcpy(out, "und", 4);
  if (input.size() > 63) return false;
  while (!input.empty() && space(input.front())) input.remove_prefix(1);
  while (!input.empty() && space(input.back())) input.remove_suffix(1);
  if (input.empty()) return false;
  char primary[4]{};
  size_t length = 0;
  bool first = true;
  for (size_t i = 0; i <= input.size(); ++i) {
    const char c = i == input.size() ? '-' : lower(input[i]);
    if (c == '-' || c == '_') {
      if (!length || length > 8 || (first && (length < 2 || length > 3))) return false;
      first = false;
      length = 0;
    } else {
      if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
      if (first) {
        if (!alpha(c) || length >= 3) return false;
        primary[length] = c;
      }
      ++length;
    }
  }
  struct Alias {
    char from[4];
    char to[4];
  };
  static constexpr Alias aliases[] = {{"eng", "en"}, {"spa", "es"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"},
                                      {"ger", "de"}, {"jpn", "ja"}, {"zho", "zh"}, {"chi", "zh"}, {"ita", "it"},
                                      {"por", "pt"}, {"rus", "ru"}, {"kor", "ko"}};
  const auto alias = std::find_if(std::begin(aliases), std::end(aliases),
                                  [&](const Alias& candidate) { return same(primary, candidate.from); });
  memcpy(out, alias != std::end(aliases) ? alias->to : primary, 4);
  return true;
}
bool validateReadingLanguageTotals(const ReadingLanguageTotals& totals) {
  if (!same(totals.entries[0].tag, "und") || !same(totals.entries[1].tag, "mul")) return false;
  bool hole = false;
  for (unsigned i = 2; i < 8; ++i) {
    const auto& e = totals.entries[i];
    if (!e.tag[0]) {
      if (e.seconds || e.tag[1] || e.tag[2] || e.tag[3]) return false;
      hole = true;
      continue;
    }
    if (hole || e.tag[3] || !alpha(e.tag[0]) || !alpha(e.tag[1]) || (e.tag[2] && !alpha(e.tag[2]))) return false;
    char normalized[4];
    if (!normalizeReadingLanguage(std::string_view(e.tag, e.tag[2] ? 3 : 2), normalized) || !same(normalized, e.tag))
      return false;
    for (unsigned j = 0; j < i; ++j)
      if (same(e.tag, totals.entries[j].tag)) return false;
  }
  return true;
}
uint8_t addReadingLanguageSeconds(ReadingLanguageTotals& totals, const char* tag, uint32_t seconds) {
  char normalized[4];
  normalizeReadingLanguage(tag ? std::string_view(tag, strnlen(tag, 4)) : std::string_view{}, normalized);
  unsigned slot = 1;
  for (unsigned i = 0; i < 8; ++i)
    if (same(totals.entries[i].tag, normalized)) {
      slot = i;
      goto found;
    }
  for (unsigned i = 2; i < 8; ++i)
    if (!totals.entries[i].tag[0]) {
      slot = i;
      memcpy(totals.entries[i].tag, normalized, 4);
      break;
    }
found:
  totals.entries[slot].seconds = saturateReadingLanguageSeconds(totals.entries[slot].seconds, seconds);
  return static_cast<uint8_t>(slot);
}
void seedUnknownReadingLanguage(ReadingLanguageTotals& totals, uint32_t seconds) {
  totals = {};
  totals.entries[0].seconds = seconds;
}
void selectReadingLanguageTags(ReadingLanguageTotals& selected, const ReadingLanguageTotals& source) {
  for (unsigned i = 2; i < 8; ++i) {
    const char* tag = source.entries[i].tag;
    if (!tag[0]) continue;
    for (unsigned j = 2; j < 8; ++j) {
      if (same(selected.entries[j].tag, tag)) break;
      if (!selected.entries[j].tag[0] || strcmp(tag, selected.entries[j].tag) < 0) {
        for (unsigned k = 7; k > j; --k) selected.entries[k] = selected.entries[k - 1];
        selected.entries[j] = {};
        memcpy(selected.entries[j].tag, tag, 4);
        break;
      }
    }
  }
}
void mergeSelectedReadingLanguageTotals(ReadingLanguageTotals& target, const ReadingLanguageTotals& source) {
  for (unsigned i = 0; i < 8; ++i) {
    if (!source.entries[i].tag[0]) continue;
    unsigned slot = i < 2 ? i : 1;
    for (unsigned j = 2; j < 8 && i >= 2; ++j)
      if (same(source.entries[i].tag, target.entries[j].tag)) {
        slot = j;
        break;
      }
    target.entries[slot].seconds =
        saturateReadingLanguageSeconds(target.entries[slot].seconds, source.entries[i].seconds);
  }
}
bool decodeReadingLanguageTotals(const uint8_t* bytes, ReadingLanguageTotals& totals) {
  for (auto& entry : totals.entries) {
    memcpy(entry.tag, bytes, 4);
    entry.seconds = le32(bytes + 4);
    bytes += 8;
  }
  return validateReadingLanguageTotals(totals);
}
bool readReadingLanguageTotals(HalFile& file, ReadingLanguageTotals& totals) {
  uint8_t bytes[8];
  for (auto& entry : totals.entries) {
    if (!readExact(file, bytes, 8)) return false;
    memcpy(entry.tag, bytes, 4);
    entry.seconds = le32(bytes + 4);
  }
  return validateReadingLanguageTotals(totals);
}
bool writeReadingLanguageTotals(HalFile& file, const ReadingLanguageTotals& totals) {
  if (!validateReadingLanguageTotals(totals)) return false;
  uint8_t bytes[8];
  for (const auto& entry : totals.entries) {
    memcpy(bytes, entry.tag, 4);
    put32(bytes + 4, entry.seconds);
    if (!writeExact(file, bytes, 8)) return false;
  }
  return true;
}
bool inspectReadingLanguageFile(HalFile& file, bool book, ReadingStatsParseMode mode, ReadingLanguageFileInfo& info) {
  info = {};
  const uint64_t size = file.fileSize64();
  if (size > (book ? 26429u : 26515u)) return false;
  info.size = static_cast<size_t>(size);
  if (!file.seekSet(0) || !readExact(file, &info.version, 1)) return false;
  info.summarySize = summarySize(info.version, book);
  if (!info.summarySize || info.size < info.summarySize) return false;
  const bool current = info.version == (book ? 6 : 4);
  if (!current) return info.size == info.summarySize;
  ReadingLanguageTotals totals;
  if (!file.seekSet(info.summarySize - 64) || !readReadingLanguageTotals(file, totals)) return false;
  if (info.size == info.summarySize) return true;
  if (mode == ReadingStatsParseMode::SyncedSummary) return false;
  uint8_t header[12];
  if (!readExact(file, header, 12) || memcmp(header, "LDAY", 4) || header[4] != 1 || header[5]) return false;
  info.rows = uint16_t(header[6]) | uint16_t(header[7]) << 8;
  info.anchor = le32(header + 8);
  info.hasAppendix = true;
  if (info.rows > kMaxRows || info.anchor > kLastDay || (info.rows && !info.anchor) ||
      info.size != info.summarySize + 12u + 36u * info.rows)
    return false;
  DayRow row;
  uint32_t previous = 0;
  for (unsigned i = 0; i < info.rows; ++i) {
    if (!readRow(file, row) || row.day <= previous || row.day < windowStart(info.anchor) || row.day > info.anchor)
      return false;
    for (unsigned j = 2; j < 8; ++j)
      if (!totals.entries[j].tag[0] && row.seconds[j]) return false;
    previous = row.day;
  }
  return true;
}
bool validateGlobalReadingStatsSummary(const uint8_t* data, size_t size) {
  if (!data || !size || size != summarySize(data[0], false)) return false;
  if (data[0] != 4) return true;
  ReadingLanguageTotals totals;
  return decodeReadingLanguageTotals(data + 159, totals);
}
bool readGlobalReadingStatsSummary(const char* path, uint8_t* out, uint8_t& size) {
  size = 0;
  HalFile file;
  ReadingLanguageFileInfo info;
  if (!Storage.openFileForRead("LSTATS", path, file)) return false;
  bool ok = inspectReadingLanguageFile(file, false, ReadingStatsParseMode::Local, info) && file.seekSet(0) &&
            readExact(file, out, info.summarySize);
  if (!file.close()) ok = false;
  if (ok) size = static_cast<uint8_t>(info.summarySize);
  return ok;
}
bool publishReadingLanguageFile(const char* path, const char* backupPath, const char* oldPath, bool book,
                                const ReadingLanguageTotals& totals, const ReadingLanguageSpan* span,
                                ReadingStatsPrefixWriter writer, const void* stats, bool keepBackup) {
  if (!validateReadingLanguageTotals(totals)) {
    LOG_ERR("LSTATS", "Invalid language summary");
    return false;
  }
  char tmp[96];
  const int tmpLength = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if (tmpLength < 0 || static_cast<size_t>(tmpLength) >= sizeof(tmp)) {
    LOG_ERR("LSTATS", "Stats path too long");
    return false;
  }
  if (Storage.exists(tmp) && !Storage.remove(tmp)) return false;
  size_t size = 0;
  if (!writeTemp(tmp, oldPath, book, totals, span, writer, stats, size)) {
    LOG_ERR("LSTATS", "Failed writing stats temp: %s", tmp);
    Storage.remove(tmp);
    return false;
  }
  if (!verifyTemp(tmp, book, size)) {
    LOG_ERR("LSTATS", "Invalid written stats: %s", tmp);
    Storage.remove(tmp);
    return false;
  }
  return promoteTemp(tmp, path, backupPath, oldPath, keepBackup);
}
bool visitLocalReadingLanguageDays(const char* path, bool (*visitor)(void*, uint32_t, const ReadingLanguageTotals&),
                                   void* context) {
  if (!visitor) return false;
  HalFile file;
  if (!Storage.openFileForRead("LSTATS", path, file)) return false;
  uint8_t version = 0;
  bool ok = readExact(file, &version, 1);
  ReadingLanguageFileInfo info;
  ok = ok && (version == 6 || version == 4) &&
       inspectReadingLanguageFile(file, version == 6, ReadingStatsParseMode::Local, info);
  ReadingLanguageTotals totals;
  ok = ok && info.hasAppendix && file.seekSet(info.summarySize - 64) && readReadingLanguageTotals(file, totals) &&
       file.seekSet(info.summarySize + 12);
  DayRow row;
  for (unsigned i = 0; ok && i < info.rows; ++i) {
    ok = readRow(file, row);
    if (!ok) break;
    for (unsigned j = 0; j < 8; ++j) totals.entries[j].seconds = row.seconds[j];
    ok = visitor(context, row.day, totals);
  }
  if (!file.close()) ok = false;
  if (!ok) LOG_ERR("LSTATS", "Daily stats unavailable or visitor stopped: %s", path);
  return ok;
}
