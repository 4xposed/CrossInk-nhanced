#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_mac.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace {
enum class StatsLoadResult : uint8_t { Ok, Invalid, NewerFormat };

struct StatsLoadOutcome {
  StatsLoadResult result = StatsLoadResult::Invalid;
  uint8_t version = 0;
  size_t fileSize = 0;
};

// Binary layout v1 (13 bytes):
//   [0]     version (= 1)
//   [1-4]   totalSessions       uint32_t LE
//   [5-8]   totalReadingSeconds uint32_t LE
//   [9-12]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (17 bytes):
//   [0]      version (= 2)
//   [1-4]    totalSessions       uint32_t LE
//   [5-8]    totalReadingSeconds uint32_t LE
//   [9-12]   totalPagesTurned    uint32_t LE
//   [13-16]  completedBooks      uint32_t LE
//
// Binary layout v3 (159 bytes):
//   [0]       version (= 3)
//   [1-4]     totalSessions             uint32_t LE
//   [5-8]     totalReadingSeconds       uint32_t LE
//   [9-12]    totalPagesTurned          uint32_t LE
//   [13-16]   completedBooks            uint32_t LE
//   [17-32]   timeOfDaySeconds[4]       uint32_t LE each
//   [33-60]   dayOfWeekSeconds[7]       uint32_t LE each
//   [61-64]   readingHistoryAnchorDay   uint32_t LE
//   [65-156]  readingHistoryBits[92]    uint8_t
//   [157-158] longestReadingStreak      uint16_t LE
static constexpr uint8_t GLOBAL_STATS_VERSION = GlobalReadingStats::CURRENT_FILE_VERSION;
static constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
static constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";
static constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";
static bool s_blockDestructiveSave = false;

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void loadCommonFields(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t addSaturated(const uint32_t a, const uint32_t b) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  return max - a < b ? max : a + b;
}

void addStats(GlobalReadingStats& target, const GlobalReadingStats& source) {
  target.totalSessions = addSaturated(target.totalSessions, source.totalSessions);
  target.totalReadingSeconds = addSaturated(target.totalReadingSeconds, source.totalReadingSeconds);
  target.totalPagesTurned = addSaturated(target.totalPagesTurned, source.totalPagesTurned);
  target.completedBooks = addSaturated(target.completedBooks, source.completedBooks);
  for (size_t i = 0; i < target.timeOfDaySeconds.size(); ++i) {
    target.timeOfDaySeconds[i] = addSaturated(target.timeOfDaySeconds[i], source.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < target.dayOfWeekSeconds.size(); ++i) {
    target.dayOfWeekSeconds[i] = addSaturated(target.dayOfWeekSeconds[i], source.dayOfWeekSeconds[i]);
  }
  mergeReadingHistory(target.readingHistoryAnchorDay, target.readingHistoryBits, source.readingHistoryAnchorDay,
                      source.readingHistoryBits);
  target.longestReadingStreak = std::max(target.longestReadingStreak, source.longestReadingStreak);
}

void serializeStats(const GlobalReadingStats& stats, uint8_t* data) {
  data[0] = GLOBAL_STATS_VERSION;
  data[1] = stats.totalSessions & 0xFF;
  data[2] = (stats.totalSessions >> 8) & 0xFF;
  data[3] = (stats.totalSessions >> 16) & 0xFF;
  data[4] = (stats.totalSessions >> 24) & 0xFF;
  data[5] = stats.totalReadingSeconds & 0xFF;
  data[6] = (stats.totalReadingSeconds >> 8) & 0xFF;
  data[7] = (stats.totalReadingSeconds >> 16) & 0xFF;
  data[8] = (stats.totalReadingSeconds >> 24) & 0xFF;
  data[9] = stats.totalPagesTurned & 0xFF;
  data[10] = (stats.totalPagesTurned >> 8) & 0xFF;
  data[11] = (stats.totalPagesTurned >> 16) & 0xFF;
  data[12] = (stats.totalPagesTurned >> 24) & 0xFF;
  data[13] = stats.completedBooks & 0xFF;
  data[14] = (stats.completedBooks >> 8) & 0xFF;
  data[15] = (stats.completedBooks >> 16) & 0xFF;
  data[16] = (stats.completedBooks >> 24) & 0xFF;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    const uint32_t value = stats.timeOfDaySeconds[i];
    const int offset = 17 + static_cast<int>(i) * 4;
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    const uint32_t value = stats.dayOfWeekSeconds[i];
    const int offset = 33 + static_cast<int>(i) * 4;
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  }
  data[61] = stats.readingHistoryAnchorDay & 0xFF;
  data[62] = (stats.readingHistoryAnchorDay >> 8) & 0xFF;
  data[63] = (stats.readingHistoryAnchorDay >> 16) & 0xFF;
  data[64] = (stats.readingHistoryAnchorDay >> 24) & 0xFF;
  memcpy(data + 65, stats.readingHistoryBits.data(), stats.readingHistoryBits.size());
  data[157] = stats.longestReadingStreak & 0xFF;
  data[158] = (stats.longestReadingStreak >> 8) & 0xFF;
}

StatsLoadOutcome loadFromOpenFile(FsFile& f, GlobalReadingStats& out,
                                  ReadingStatsParseMode mode = ReadingStatsParseMode::Local) {
  StatsLoadOutcome outcome;
  ReadingLanguageFileInfo info;
  const bool valid = inspectReadingLanguageFile(f, false, mode, info);
  outcome.version = info.version;
  outcome.fileSize = info.size;
  if (info.version > GLOBAL_STATS_VERSION) {
    outcome.result = StatsLoadResult::NewerFormat;
    return outcome;
  }
  if (!valid || !f.seekSet(0)) return outcome;
  // Prefix stays 159 bytes; the eight entries are decoded individually.
  uint8_t data[159]{};
  const size_t prefix = std::min<size_t>(info.summarySize, sizeof(data));
  if (f.read(data, prefix) != static_cast<int>(prefix)) return outcome;
  loadCommonFields(data, out);
  if (info.version >= 2) out.completedBooks = readLe32(data, 13);
  if (info.version >= 3) {
    for (size_t i = 0; i < out.timeOfDaySeconds.size(); ++i) out.timeOfDaySeconds[i] = readLe32(data, 17 + i * 4);
    for (size_t i = 0; i < out.dayOfWeekSeconds.size(); ++i) out.dayOfWeekSeconds[i] = readLe32(data, 33 + i * 4);
    out.readingHistoryAnchorDay = readLe32(data, 61);
    memcpy(out.readingHistoryBits.data(), data + 65, out.readingHistoryBits.size());
    out.longestReadingStreak = readLe16(data, 157);
  }
  if (info.version == 4) {
    if (!readReadingLanguageTotals(f, out.languageTotals)) return outcome;
  } else
    seedUnknownReadingLanguage(out.languageTotals, out.totalReadingSeconds);
  outcome.result = StatsLoadResult::Ok;
  return outcome;
}

std::string localSyncedStatsFileName() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != 0) return {};

  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

bool writeGlobalPrefix(HalFile& file, const void* value) {
  uint8_t data[159];
  serializeStats(*static_cast<const GlobalReadingStats*>(value), data);
  return file.write(data, sizeof(data)) == sizeof(data);
}
}  // namespace

static StatsLoadOutcome loadFromFile(const char* path, GlobalReadingStats& out) {
  StatsLoadOutcome outcome;
  FsFile f;
  if (!Storage.openFileForRead("GSTATS", path, f)) return outcome;
  outcome = loadFromOpenFile(f, out);
  if (!f.close()) outcome.result = StatsLoadResult::Invalid;
  return outcome;
}

// Validate provenance on every mutation too: a prior UI read is not required.
static bool globalSource(const char*& source) {
  source = nullptr;
  static constexpr const char* paths[] = {GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH,
                                          "/.crosspoint/global_stats.bin.recovery"};
  bool foundInvalid = false;
  for (const char* path : paths) {
    if (!Storage.exists(path)) continue;
    FsFile file;
    if (!Storage.openFileForRead("GSTATS", path, file)) return false;
    ReadingLanguageFileInfo info;
    bool valid = inspectReadingLanguageFile(file, false, ReadingStatsParseMode::Local, info);
    if (!file.close()) valid = false;
    if (info.version > GLOBAL_STATS_VERSION) {
      s_blockDestructiveSave = true;
      return false;
    }
    if (valid) {
      source = path;
      return true;
    }
    foundInvalid = true;
  }
  return !foundInvalid;
}

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  const char* source = nullptr;
  if (!globalSource(source)) {
    LOG_ERR("GSTATS", "Global stats corrupt/newer; mutation blocked");
    stats.persistenceWritable = false;
    return stats;
  }
  if (source && loadFromFile(source, stats).result != StatsLoadResult::Ok) {
    LOG_ERR("GSTATS", "Cannot load validated global stats");
    stats.persistenceWritable = false;
    return stats;
  }
  return stats;
}

GlobalReadingStats GlobalReadingStats::loadAggregated() { return loadAggregated(load()); }

bool GlobalReadingStats::hasSyncedStats() {
  FsFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return false;

  const bool exists = dir.isDirectory();
  dir.close();
  return exists;
}

namespace {
// Two directory passes select the same six names irrespective of enumeration order.
// The per-file snapshot lives in a separate frame from the returned aggregate.
void aggregatePass(GlobalReadingStats& result, ReadingLanguageTotals& selected, const char* localName, bool select) {
  FsFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return;
  if (!dir.isDirectory()) {
    dir.close();
    return;
  }
  char name[40];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const size_t n = file.getName(name, sizeof(name));
    bool accepted = !file.isDirectory() && n == 23 && strncmp(name, "device_", 7) == 0 &&
                    strcmp(name + 19, ".bin") == 0 && strcmp(name, localName) != 0;
    for (size_t i = 7; accepted && i < 19; ++i)
      accepted = (name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f');
    if (accepted) {
      GlobalReadingStats source;
      const auto outcome = loadFromOpenFile(file, source, ReadingStatsParseMode::SyncedSummary);
      if (outcome.result == StatsLoadResult::Ok) {
        if (select)
          selectReadingLanguageTags(selected, source.languageTotals);
        else {
          addStats(result, source);
          mergeSelectedReadingLanguageTotals(result.languageTotals, source.languageTotals);
        }
      } else
        LOG_ERR("GSTATS", "Skipped invalid synced summary: %s", name);
    }
    file.close();
  }
  dir.close();
}
}  // namespace
GlobalReadingStats GlobalReadingStats::loadAggregated(const GlobalReadingStats& localStats) {
  GlobalReadingStats result = localStats;
  const std::string localName = localSyncedStatsFileName();
  if (localName.empty()) {
    LOG_ERR("GSTATS", "Device MAC unavailable; returning local stats only");
    return result;
  }
  ReadingLanguageTotals selected;
  selectReadingLanguageTags(selected, localStats.languageTotals);
  aggregatePass(result, selected, localName.c_str(), true);
  result.languageTotals = selected;
  mergeSelectedReadingLanguageTotals(result.languageTotals, localStats.languageTotals);
  aggregatePass(result, selected, localName.c_str(), false);
  return result;
}
bool GlobalReadingStats::save(const ReadingLanguageSpan* span) const {
  const char* source = nullptr;
  if (!persistenceWritable || s_blockDestructiveSave || !globalSource(source)) {
    LOG_ERR("GSTATS", "Refusing unsafe global stats overwrite");
    return false;
  }
  return publishReadingLanguageFile(GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH, source, false, languageTotals, span,
                                    writeGlobalPrefix, this, source && strcmp(source, GLOBAL_STATS_PATH) != 0);
}
bool GlobalReadingStats::resetLocal() {
  const GlobalReadingStats empty;
  const bool ok = publishReadingLanguageFile(GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH, nullptr, false,
                                             empty.languageTotals, nullptr, writeGlobalPrefix, &empty, true);
  if (ok) s_blockDestructiveSave = false;
  return ok;
}

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);

  const uint16_t historyLongest = computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits);
  if (historyLongest > longestReadingStreak) {
    longestReadingStreak = historyLongest;
  }
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}
