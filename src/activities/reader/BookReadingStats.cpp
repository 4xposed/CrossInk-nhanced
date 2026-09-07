#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
// Binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (12 bytes):
//   [0]     version (= 2)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//   [11]    isCompleted         uint8_t
//
// Binary layout v3 (16 bytes):
//   [0]      version (= 3)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Binary layout v4 (69 bytes):
//   [0]      version (= 4)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
static constexpr uint8_t STATS_FILE_VERSION = 6;
static constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
static constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
static constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
static constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
static constexpr int STATS_FILE_SIZE_V1 = 11;
static constexpr int STATS_FILE_SIZE_V2 = 12;
static constexpr int STATS_FILE_SIZE_V3 = 16;
static constexpr int STATS_FILE_SIZE_V4 = 69;
static constexpr int STATS_FILE_SIZE = 73;
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
static constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
// Find a validated primary, recovery, or explicit migration source. Existing corrupt
// or newer data blocks mutation; a failed open is not the same as a missing file.
bool findStatsPath(const std::string& cachePath, char (&path)[96]) {
  static constexpr const char* names[] = {"stats_v6.bin", "stats_v6.bin.bak", "stats_v6.bin.recovery",
                                          "stats_v5.bin", "stats_v4.bin",     "stats.bin"};
  bool invalid = false;
  for (unsigned index = 0; index < 6; ++index) {
    const char* name = names[index];
    if (invalid && index >= 3) return false;
    char candidate[96];
    const int length = snprintf(candidate, sizeof(candidate), "%s/%s", cachePath.c_str(), name);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(candidate)) {
      LOG_ERR("STATS", "Stats cache path too long");
      return false;
    }
    if (!Storage.exists(candidate)) continue;
    FsFile file;
    if (!Storage.openFileForRead("STATS", candidate, file)) return false;
    ReadingLanguageFileInfo info;
    const bool valid = inspectReadingLanguageFile(file, true, ReadingStatsParseMode::Local, info);
    const bool closed = file.close();
    if (info.version > STATS_FILE_VERSION) {
      LOG_ERR("STATS", "Newer book stats; refusing overwrite");
      return false;
    }
    if (valid && closed) {
      memcpy(path, candidate, static_cast<size_t>(length) + 1);
      return true;
    }
    // Do not silently roll back into legacy files after corrupt v6 history.
    if (name == names[2] || (invalid && name == names[3])) return false;
    invalid = true;
  }
  path[0] = 0;
  return !invalid;
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  char path[96]{};
  if (!findStatsPath(cachePath, path)) {
    stats.persistenceWritable = false;
    return stats;
  }
  if (!path[0]) return stats;
  FsFile f;
  if (!Storage.openFileForRead("STATS", path, f)) {
    stats.persistenceWritable = false;
    return stats;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const size_t expected = std::min<size_t>(f.fileSize(), STATS_FILE_SIZE);
  const int n = f.read(data, expected);
  if (n != static_cast<int>(expected)) {
    f.close();
    stats.persistenceWritable = false;
    return stats;
  }
  if (data[0] == STATS_FILE_VERSION) {
    if (!readReadingLanguageTotals(f, stats.languageTotals)) {
      f.close();
      stats.persistenceWritable = false;
      return stats;
    }
  } else {
    seedUnknownReadingLanguage(stats.languageTotals, n >= 7 ? readLe32(data, 3) : 0);
  }
  if (!f.close()) {
    stats.persistenceWritable = false;
    return stats;
  }

  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, stats);
    return stats;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    return stats;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    stats.avgSecondsPerForwardPage = readLe16(data, 12);
    stats.paceSampleCount = readLe16(data, 14);
    return stats;
  }

  if (n != STATS_FILE_SIZE && n != STATS_FILE_SIZE_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] != STATS_FILE_VERSION_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE && data[0] != 5 && data[0] != STATS_FILE_VERSION) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (n == STATS_FILE_SIZE) {
    stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  return stats;
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

namespace {
bool writeBookPrefix(HalFile& f, const void* value) {
  const auto& stats = *static_cast<const BookReadingStats*>(value);
  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, stats.sessionCount);
  writeLe32(data, 3, stats.totalReadingSeconds);
  writeLe32(data, 7, stats.totalPagesTurned);
  data[11] = stats.isCompleted ? 1 : 0;
  writeLe16(data, 12, stats.avgSecondsPerForwardPage);
  writeLe16(data, 14, stats.paceSampleCount);
  data[16] = (stats.startDateManual ? FLAG_START_DATE_MANUAL : 0u) |
             (stats.finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, stats.startDate.isValid() ? stats.startDate.year : 0);
  data[19] = stats.startDate.isValid() ? stats.startDate.month : 0;
  data[20] = stats.startDate.isValid() ? stats.startDate.day : 0;
  writeLe16(data, 21, stats.finishedDate.isValid() ? stats.finishedDate.year : 0);
  data[23] = stats.finishedDate.isValid() ? stats.finishedDate.month : 0;
  data[24] = stats.finishedDate.isValid() ? stats.finishedDate.day : 0;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, stats.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, stats.dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, stats.estimatedTimeLeftSeconds);
  return f.write(data, STATS_FILE_SIZE) == STATS_FILE_SIZE;
}
}  // namespace
bool BookReadingStats::save(const std::string& cachePath, const ReadingLanguageSpan* span) const {
  if (!persistenceWritable) {
    LOG_ERR("STATS", "Refusing save of failed-load snapshot");
    return false;
  }
  char source[96]{};
  if (!findStatsPath(cachePath, source)) {
    LOG_ERR("STATS", "Book stats cannot be safely mutated");
    return false;
  }
  char path[96];
  const int length = snprintf(path, sizeof(path), "%s/stats_v6.bin", cachePath.c_str());
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    LOG_ERR("STATS", "Stats cache path too long");
    return false;
  }
  const bool recovered = source[0] && strcmp(source, path) != 0 && strstr(source, "stats_v6.bin.");
  return publishReadingLanguageFile(path, nullptr, source[0] ? source : nullptr, true, languageTotals, span,
                                    writeBookPrefix, this, recovered);
}

bool BookReadingStats::remove(const std::string& cachePath) {
  static constexpr const char* names[] = {"stats_v6.bin", "stats_v5.bin", "stats_v4.bin", "stats.bin"};
  static constexpr const char* suffixes[] = {"", ".tmp", ".bak", ".recovery", ".invalid"};
  bool ok = true;
  for (const char* name : names)
    for (const char* suffix : suffixes) {
      const std::string path = cachePath + "/" + name + suffix;
      if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
        LOG_ERR("STATS", "Could not remove %s", path.c_str());
        ok = false;
      }
    }
  return ok;
}
