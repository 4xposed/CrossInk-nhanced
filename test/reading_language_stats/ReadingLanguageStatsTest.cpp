#include <HalStorage.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "test/UniqueTempDirectory.h"
class LanguageStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    storage_test::root = uniqueTempDirectory("crossink-language-stats-fixture").string();
    std::filesystem::create_directories(storage_test::root + "/.crosspoint/book");
  }
  void TearDown() override {
    EXPECT_EQ(storage_test::openFiles, 0);
    std::filesystem::remove_all(storage_test::root);
  }
};
TEST_F(LanguageStatsTest, BookPublishesVersionSixWithEmptyAppendix) {
  BookReadingStats stats;
  stats.totalReadingSeconds = 123;
  stats.save("/.crosspoint/book");
  EXPECT_TRUE(Storage.exists("/.crosspoint/book/stats_v6.bin"));
}
TEST_F(LanguageStatsTest, GlobalPublishesVersionFourWithEmptyAppendix) {
  GlobalReadingStats stats;
  stats.save();
  std::ifstream f(storage_test::root + "/.crosspoint/global_stats.bin", std::ios::binary);
  EXPECT_EQ(f.get(), 4);
  EXPECT_EQ(std::filesystem::file_size(storage_test::root + "/.crosspoint/global_stats.bin"), 235u);
}

#include <esp_mac.h>

#include <array>
#include <map>
#include <vector>

#include "ReadingLanguageStats.h"
namespace {
constexpr const char* bookPath = "/.crosspoint/book/stats_v6.bin";
constexpr const char* globalPath = "/.crosspoint/global_stats.bin";
std::vector<uint8_t> bytes(const char* path) {
  std::ifstream f(storage_test::mapped(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
void put(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = v >> (8 * i);
}
void writeBytes(const char* path, const std::vector<uint8_t>& b) {
  std::ofstream f(storage_test::mapped(path), std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), b.size());
}
uint32_t get(const std::vector<uint8_t>& b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
ReadingLanguageSpan span(const char* lang, uint32_t seconds, ReadingStatsDate date = {2024, 2, 28}, uint8_t hour = 23,
                         uint8_t minute = 59, uint8_t second = 50) {
  ReadingLanguageSpan s;
  s.localStart = {date, hour, minute, second};
  s.seconds = seconds;
  normalizeReadingLanguage(lang, s.normalizedTag);
  return s;
}
void add(BookReadingStats& b, GlobalReadingStats& g, const ReadingLanguageSpan& s) {
  b.totalReadingSeconds += s.seconds;
  g.totalReadingSeconds += s.seconds;
  addReadingLanguageSeconds(b.languageTotals, s.normalizedTag, s.seconds);
  addReadingLanguageSeconds(g.languageTotals, s.normalizedTag, s.seconds);
}
using Days = std::map<uint32_t, ReadingLanguageTotals>;
bool collect(void* ctx, uint32_t day, const ReadingLanguageTotals& totals) {
  return static_cast<Days*>(ctx)->emplace(day, totals).second;
}
Days days(const char* path) {
  Days d;
  EXPECT_TRUE(visitLocalReadingLanguageDays(path, collect, &d));
  return d;
}
std::vector<uint8_t> fixture(bool book, int rows = 0) {
  std::vector<uint8_t> b((book ? 137 : 223) + 12 + 36 * rows);
  b[0] = book ? 6 : 4;
  const size_t prefix = book ? 73 : 159;
  memcpy(b.data() + prefix, "und", 4);
  memcpy(b.data() + prefix + 8, "mul", 4);
  memcpy(b.data() + prefix + 16, "en", 3);
  const size_t h = prefix + 64;
  memcpy(b.data() + h, "LDAY", 4);
  b[h + 4] = 1;
  b[h + 6] = rows & 255;
  b[h + 7] = rows >> 8;
  put(b, h + 8, rows ? 9000 : 0);
  for (int i = 0; i < rows; ++i) {
    put(b, h + 12 + 36 * i, 9001 - rows + i);
    put(b, h + 12 + 36 * i + 4 + 8, 60);
  }
  return b;
}
}  // namespace
TEST_F(LanguageStatsTest, NormalizesBoundedMetadataAndAliases) {
  const std::pair<const char*, const char*> cases[] = {
      {" EN_us ", "en"}, {"zh-Hant", "zh"}, {"fra", "fr"},      {"fre", "fr"},           {"GER", "de"},
      {"jpn", "ja"},     {"chi", "zh"},     {"ita", "it"},      {"por", "pt"},           {"rus", "ru"},
      {"kor", "ko"},     {"xyz", "xyz"},    {"mul", "mul"},     {"und", "und"},          {"x-private", "und"},
      {"en--US", "und"}, {"e", "und"},      {"english", "und"}, {"en-123456789", "und"}, {"en-", "und"},
      {"en/US", "und"},  {"", "und"}};
  for (const auto& c : cases) {
    char tag[4];
    normalizeReadingLanguage(c.first, tag);
    EXPECT_STREQ(tag, c.second) << c.first;
  }
  char tag[4];
  EXPECT_FALSE(normalizeReadingLanguage(std::string(64, 'a'), tag));
  EXPECT_STREQ(tag, "und");
}
TEST_F(LanguageStatsTest, CapacityOverflowIsOtherAndCountersSaturate) {
  ReadingLanguageTotals totals;
  for (const char* lang : {"en", "es", "fr", "de", "ja", "zh"}) addReadingLanguageSeconds(totals, lang, 60);
  EXPECT_EQ(addReadingLanguageSeconds(totals, "it", 70), 1);
  addReadingLanguageSeconds(totals, "und", 80);
  EXPECT_EQ(totals.entries[0].seconds, 80u);
  EXPECT_EQ(totals.entries[1].seconds, 70u);
  addReadingLanguageSeconds(totals, "en", UINT32_MAX);
  addReadingLanguageSeconds(totals, "en", 5);
  EXPECT_EQ(totals.entries[2].seconds, UINT32_MAX);
  EXPECT_EQ(sizeof(totals), 64u);
  EXPECT_LT(sizeof(GlobalReadingStats), 256u);
}
TEST_F(LanguageStatsTest, AllLegacyBookVersionsMigrateUnknownWithoutDays) {
  const int sizes[] = {0, 11, 12, 16, 69, 73};
  for (int v = 1; v <= 5; ++v) {
    ASSERT_TRUE(BookReadingStats::remove("/.crosspoint/book"));
    std::vector<uint8_t> b(sizes[v]);
    b[0] = v;
    put(b, 3, 1234);
    const char* path = v == 5   ? "/.crosspoint/book/stats_v5.bin"
                       : v == 4 ? "/.crosspoint/book/stats_v4.bin"
                                : "/.crosspoint/book/stats.bin";
    writeBytes(path, b);
    auto stats = BookReadingStats::load("/.crosspoint/book");
    EXPECT_EQ(stats.totalReadingSeconds, 1234u) << v;
    EXPECT_EQ(stats.languageTotals.entries[0].seconds, 1234u) << v;
    ASSERT_TRUE(stats.save("/.crosspoint/book"));
    EXPECT_TRUE(days(bookPath).empty());
    EXPECT_EQ(bytes(bookPath).size(), 149u);
  }
}
TEST_F(LanguageStatsTest, AllLegacyGlobalVersionsMigrateUnknownWithoutDays) {
  const int sizes[] = {0, 13, 17, 159};
  for (int v = 1; v <= 3; ++v) {
    std::vector<uint8_t> b(sizes[v]);
    b[0] = v;
    put(b, 5, 4567);
    writeBytes(globalPath, b);
    auto stats = GlobalReadingStats::load();
    EXPECT_EQ(stats.totalReadingSeconds, 4567u);
    EXPECT_EQ(stats.languageTotals.entries[0].seconds, 4567u);
    ASSERT_TRUE(stats.save());
    EXPECT_TRUE(days(globalPath).empty());
  }
}
TEST_F(LanguageStatsTest, MixedSessionsSplitLeapMidnightAndMetadataSavePreservesBytes) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("EN-us", 30);
  add(b, g, s);
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  ASSERT_TRUE(g.save(&s));
  auto d = days(bookPath);
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d.at(8824).entries[2].seconds, 10u);
  EXPECT_EQ(d.at(8825).entries[2].seconds, 20u);
  auto next = span("ja", 90, {2024, 2, 29}, 12, 0, 0);
  add(b, g, next);
  ASSERT_TRUE(b.save("/.crosspoint/book", &next));
  ASSERT_TRUE(g.save(&next));
  auto before = bytes(bookPath);
  b.isCompleted = true;
  b.finishedDate = {2024, 3, 1};
  ASSERT_TRUE(b.save("/.crosspoint/book"));
  auto after = bytes(bookPath);
  EXPECT_EQ(std::vector<uint8_t>(before.begin() + 73, before.end()),
            std::vector<uint8_t>(after.begin() + 73, after.end()));
  auto restored = BookReadingStats::load("/.crosspoint/book");
  EXPECT_EQ(restored.languageTotals.entries[2].seconds, 30u);
  EXPECT_EQ(restored.languageTotals.entries[3].seconds, 90u);
  EXPECT_EQ(restored.totalReadingSeconds, 120u);
  EXPECT_EQ(days(globalPath).at(8825).entries[3].seconds, 90u);
}
TEST_F(LanguageStatsTest, MaximumAppendicesRoundTripByteExactly) {
  for (bool book : {false, true}) {
    auto b = fixture(book, 730);
    const char* path = book ? bookPath : globalPath;
    writeBytes(path, b);
    if (book) {
      auto s = BookReadingStats::load("/.crosspoint/book");
      ASSERT_TRUE(s.save("/.crosspoint/book"));
    } else {
      auto s = GlobalReadingStats::load();
      ASSERT_TRUE(s.save());
    }
    EXPECT_EQ(bytes(path), b);
    EXPECT_EQ(days(path).size(), 730u);
    EXPECT_EQ(b.size(), book ? 26429u : 26515u);
  }
}
TEST_F(LanguageStatsTest, InvalidRtcAndZeroIntervalsCreateNoDays) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("es", 90);
  s.localStart = {};
  add(b, g, s);
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  EXPECT_TRUE(days(bookPath).empty());
  EXPECT_EQ(BookReadingStats::load("/.crosspoint/book").languageTotals.entries[2].seconds, 90u);
  s = span("es", 0);
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  EXPECT_TRUE(days(bookPath).empty());
  s = span("es", 1);
  s.localStart.hour = 24;
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  EXPECT_TRUE(days(bookPath).empty());
}
TEST_F(LanguageStatsTest, RollbackRetainsAnchorAndExpiryNeverSubtractsLifetime) {
  auto data = fixture(true, 730);
  writeBytes(bookPath, data);
  auto b = BookReadingStats::load("/.crosspoint/book");
  auto s = span("en", 20, {2022, 1, 1}, 0, 0, 0);
  b.totalReadingSeconds += 20;
  addReadingLanguageSeconds(b.languageTotals, "en", 20);
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  EXPECT_EQ(get(bytes(bookPath), 145), 9000u);
  EXPECT_EQ(days(bookPath).size(), 730u);
  EXPECT_EQ(b.languageTotals.entries[2].seconds, 20u);
  s = span("en", 10, {2026, 1, 1}, 0, 0, 0);
  addReadingLanguageSeconds(b.languageTotals, "en", 10);
  ASSERT_TRUE(b.save("/.crosspoint/book", &s));
  auto d = days(bookPath);
  EXPECT_EQ(d.size(), 234u);
  EXPECT_EQ(b.languageTotals.entries[2].seconds, 30u);
}
TEST_F(LanguageStatsTest, MonthAndYearBoundarySpansKeepExactSeconds) {
  for (auto date : {ReadingStatsDate{2023, 12, 31}, ReadingStatsDate{2024, 2, 29}}) {
    ASSERT_TRUE(BookReadingStats::remove("/.crosspoint/book"));
    BookReadingStats b;
    auto s = span("fr", 20, date);
    addReadingLanguageSeconds(b.languageTotals, "fr", 20);
    ASSERT_TRUE(b.save("/.crosspoint/book", &s));
    auto d = days(bookPath);
    ASSERT_EQ(d.size(), 2u);
    for (const auto& day : d) EXPECT_EQ(day.second.entries[2].seconds, 10u);
  }
}
TEST_F(LanguageStatsTest, ShortWritesSyncCloseAndRenameFailuresRetainDurableData) {
  for (int failure = 0; failure < 5; ++failure) {
    ASSERT_TRUE(BookReadingStats::remove("/.crosspoint/book"));
    BookReadingStats b;
    auto s = span("en", 20);
    addReadingLanguageSeconds(b.languageTotals, "en", 20);
    ASSERT_TRUE(b.save("/.crosspoint/book", &s));
    const auto before = bytes(bookPath);
    auto next = span("en", 30);
    addReadingLanguageSeconds(b.languageTotals, "en", 30);
    if (failure == 0) storage_test::failWriteCall = storage_test::writeCalls + 1;
    if (failure == 1) storage_test::failSyncCall = storage_test::syncCalls + 1;
    if (failure == 2) storage_test::failCloseCall = storage_test::closeCalls + 3;
    if (failure == 3) storage_test::failRenameCall = storage_test::renameCalls + 1;
    if (failure == 4) storage_test::failRenameCall = storage_test::renameCalls + 2;
    EXPECT_FALSE(b.save("/.crosspoint/book", &next)) << failure;
    EXPECT_EQ(bytes(bookPath), before) << failure;
    ASSERT_TRUE(b.save("/.crosspoint/book", &next));
    EXPECT_EQ(days(bookPath).at(8825).entries[2].seconds, 30u);
  }
}
TEST_F(LanguageStatsTest, CorruptPrimaryRecoversBackupWithoutRotatingCorruptionOverIt) {
  GlobalReadingStats g;
  auto s = span("ja", 20);
  addReadingLanguageSeconds(g.languageTotals, "ja", 20);
  ASSERT_TRUE(g.save(&s));
  ASSERT_TRUE(g.save());
  const auto backup = bytes("/.crosspoint/global_stats.bin.bak");
  writeBytes(globalPath, {4, 0});
  g = GlobalReadingStats::load();
  EXPECT_EQ(g.languageTotals.entries[2].seconds, 20u);
  storage_test::failRenameCall = storage_test::renameCalls + 2;
  EXPECT_FALSE(g.save());
  EXPECT_EQ(bytes("/.crosspoint/global_stats.bin.bak"), backup);
  ASSERT_TRUE(g.save());
  EXPECT_EQ(bytes(globalPath), backup);
}
TEST_F(LanguageStatsTest, ResetClearsBothLocalDimensionsAndKeepsBackupAndRemote) {
  GlobalReadingStats g;
  auto s = span("ja", 20);
  addReadingLanguageSeconds(g.languageTotals, "ja", 20);
  ASSERT_TRUE(g.save(&s));
  ASSERT_TRUE(g.save());
  auto backup = bytes("/.crosspoint/global_stats.bin.bak");
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(GlobalReadingStats::load().languageTotals.entries[2].seconds, 0u);
  EXPECT_TRUE(days(globalPath).empty());
  EXPECT_EQ(bytes("/.crosspoint/global_stats.bin.bak"), backup);
}
TEST_F(LanguageStatsTest, CorruptAppendixAndNewerFilesRefuseMutation) {
  auto valid = fixture(true, 2);
  for (size_t len : {size_t(0), size_t(73), size_t(136), size_t(138), size_t(148), valid.size() - 1}) {
    auto b = valid;
    b.resize(len);
    writeBytes(bookPath, b);
    BookReadingStats stats;
    EXPECT_FALSE(stats.save("/.crosspoint/book")) << len;
    EXPECT_EQ(bytes(bookPath), b);
  }
  for (size_t at :
       {size_t(73), size_t(80), size_t(92), size_t(141), size_t(142), size_t(143), size_t(149), size_t(185)}) {
    auto b = valid;
    b[at] = 255;
    writeBytes(bookPath, b);
    BookReadingStats stats;
    EXPECT_FALSE(stats.save("/.crosspoint/book")) << at;
  }
  writeBytes(bookPath, {7});
  EXPECT_FALSE(BookReadingStats{}.save("/.crosspoint/book"));
  EXPECT_EQ(bytes(bookPath), std::vector<uint8_t>{7});
  writeBytes(globalPath, {5});
  EXPECT_FALSE(GlobalReadingStats{}.save());
  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_TRUE(GlobalReadingStats{}.save());
}
TEST_F(LanguageStatsTest, LargeLocalFileExportsOnlySummaryAndSyncedModeRejectsAppendix) {
  auto data = fixture(false, 730);
  writeBytes(globalPath, data);
  uint8_t buffer[223];
  uint8_t size = 0;
  ASSERT_TRUE(readGlobalReadingStatsSummary(globalPath, buffer, size));
  EXPECT_EQ(size, 223);
  EXPECT_TRUE(std::equal(buffer, buffer + 223, data.begin()));
  EXPECT_TRUE(validateGlobalReadingStatsSummary(buffer, size));
  EXPECT_FALSE(validateGlobalReadingStatsSummary(data.data(), data.size()));
}
TEST_F(LanguageStatsTest, AggregationSelectsSixLexicalTagsAndSkipsSelfAndFullLocalCopies) {
  std::filesystem::create_directories(storage_test::root + "/.crosspoint/synced_stats");
  GlobalReadingStats local;
  local.totalReadingSeconds = 60;
  addReadingLanguageSeconds(local.languageTotals, "zh", 60);
  for (int i = 0; i < 2; ++i) {
    auto b = fixture(false);
    b.resize(223);
    put(b, 5, 200);
    const char* tags[2][6] = {{"en", "ja", "ru", "es", "it", "de"}, {"fr", "ko", "pt", "en", "ar", "nl"}};
    for (int j = 0; j < 6; ++j) {
      memcpy(b.data() + 159 + 8 * (j + 2), tags[i][j], 3);
      put(b, 159 + 8 * (j + 2) + 4, 10);
    }
    writeBytes(
        i ? "/.crosspoint/synced_stats/device_222222222222.bin" : "/.crosspoint/synced_stats/device_111111111111.bin",
        b);
  }
  auto self = fixture(false);
  self.resize(223);
  put(self, 5, 999);
  writeBytes("/.crosspoint/synced_stats/device_010203040506.bin", self);
  writeBytes("/.crosspoint/synced_stats/device_333333333333.bin", fixture(false, 1));
  auto all = GlobalReadingStats::loadAggregated(local);
  EXPECT_EQ(all.totalReadingSeconds, 460u);
  const char* want[] = {"ar", "de", "en", "es", "fr", "it"};
  for (int i = 0; i < 6; ++i) EXPECT_STREQ(all.languageTotals.entries[i + 2].tag, want[i]);
  EXPECT_EQ(all.languageTotals.entries[4].seconds, 20u);
  EXPECT_EQ(all.languageTotals.entries[1].seconds, 110u);
  failMac = true;
  EXPECT_EQ(GlobalReadingStats::loadAggregated(local).totalReadingSeconds, 60u);
  failMac = false;
}
#include "ReadingStatsSave.h"
TEST_F(LanguageStatsTest, FirstAttemptSuccessDoesNotReplayEitherSpan) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("en", 30);
  add(b, g, s);
  auto saved = saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s);
  ASSERT_TRUE(saved.complete());
  EXPECT_EQ(days(bookPath).at(8825).entries[2].seconds, 20u);
  EXPECT_EQ(days(globalPath).at(8825).entries[2].seconds, 20u);
  const int writes = storage_test::writeCalls;
  EXPECT_TRUE(saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s, saved).complete());
  EXPECT_EQ(storage_test::writeCalls, writes);
}
TEST_F(LanguageStatsTest, OneRetryOnlyForFailedBookOrGlobalTarget) {
  for (bool failBook : {true, false}) {
    ASSERT_TRUE(BookReadingStats::remove("/.crosspoint/book"));
    ASSERT_TRUE(GlobalReadingStats::resetLocal());
    BookReadingStats b;
    GlobalReadingStats g;
    auto s = span("ja", 30);
    add(b, g, s);
    storage_test::failSyncCall = storage_test::syncCalls + (failBook ? 1 : 2);
    ASSERT_TRUE(saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s).complete());
    EXPECT_EQ(days(bookPath).at(8825).entries[2].seconds, 20u);
    EXPECT_EQ(days(globalPath).at(8825).entries[2].seconds, 20u);
  }
}
TEST_F(LanguageStatsTest, PersistentFailureRemainsIncompleteAfterExactlyOneRetry) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("ja", 30);
  add(b, g, s);
  storage_test::failWrite = true;
  const int start = storage_test::writeCalls;
  auto saved = saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s);
  EXPECT_FALSE(saved.book);
  EXPECT_FALSE(saved.global);
  EXPECT_EQ(storage_test::writeCalls - start, 4);
  storage_test::failWrite = false;
}
TEST_F(LanguageStatsTest, SecondOpenFailureCannotTurnDurableStatsIntoWritableEmptySnapshot) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("und", 30);
  add(b, g, s);
  ASSERT_TRUE(saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s).complete());
  const auto beforeBook = bytes(bookPath), beforeGlobal = bytes(globalPath);
  storage_test::failOpenReadCall = storage_test::openReadCalls + 2;
  auto failedBook = BookReadingStats::load("/.crosspoint/book");
  EXPECT_FALSE(failedBook.save("/.crosspoint/book"));
  EXPECT_EQ(bytes(bookPath), beforeBook);
  EXPECT_TRUE(BookReadingStats::load("/.crosspoint/book").save("/.crosspoint/book"));
  storage_test::failOpenReadCall = storage_test::openReadCalls + 2;
  auto failedGlobal = GlobalReadingStats::load();
  EXPECT_FALSE(failedGlobal.save());
  EXPECT_EQ(bytes(globalPath), beforeGlobal);
  EXPECT_TRUE(GlobalReadingStats::load().save());
}
TEST_F(LanguageStatsTest, SecondReadCloseFailureCannotOverwriteDurableUnknownHistory) {
  BookReadingStats b;
  GlobalReadingStats g;
  auto s = span("und", 30);
  add(b, g, s);
  ASSERT_TRUE(saveReadingStatsWithRetry("/.crosspoint/book", b, g, &s).complete());
  storage_test::failCloseCall = storage_test::closeCalls + 2;
  EXPECT_FALSE(BookReadingStats::load("/.crosspoint/book").save("/.crosspoint/book"));
  storage_test::failCloseCall = storage_test::closeCalls + 2;
  EXPECT_FALSE(GlobalReadingStats::load().save());
}
#include "StatsBackup.h"
TEST_F(LanguageStatsTest, BackupStreamsMaximumValidatedFileAndRetainsOldBackupOnFailure) {
  auto data = fixture(false, 730);
  writeBytes(globalPath, data);
  char name[64];
  ASSERT_TRUE(backupGlobalStats(true, name, sizeof(name)));
  auto path = std::string("/.crossink-stats-backup/") + name;
  EXPECT_EQ(bytes(path.c_str()), data);
  storage_test::failSyncCall = storage_test::syncCalls + 1;
  EXPECT_FALSE(backupGlobalStats(true, name, sizeof(name)));
  EXPECT_EQ(bytes(path.c_str()), data);
}
TEST_F(LanguageStatsTest, CorruptCurrentBookCannotSilentlyFallBackToLegacy) {
  auto old = std::vector<uint8_t>(73);
  old[0] = 5;
  put(old, 3, 100);
  writeBytes("/.crosspoint/book/stats_v5.bin", old);
  writeBytes(bookPath, {6, 0});
  EXPECT_FALSE(BookReadingStats::load("/.crosspoint/book").save("/.crosspoint/book"));
  EXPECT_EQ(bytes(bookPath), (std::vector<uint8_t>{6, 0}));
}
TEST_F(LanguageStatsTest, SummaryOnlyLocalRecoveryAddsEmptyAppendix) {
  auto b = fixture(true);
  b.resize(137);
  writeBytes(bookPath, b);
  auto book = BookReadingStats::load("/.crosspoint/book");
  ASSERT_TRUE(book.save("/.crosspoint/book"));
  EXPECT_EQ(bytes(bookPath).size(), 149u);
  EXPECT_TRUE(days(bookPath).empty());
  auto g = fixture(false);
  g.resize(223);
  writeBytes(globalPath, g);
  ASSERT_TRUE(GlobalReadingStats::load().save());
  EXPECT_EQ(bytes(globalPath).size(), 235u);
  EXPECT_TRUE(days(globalPath).empty());
}
TEST_F(LanguageStatsTest, DayCellSaturatesWithoutWrapping) {
  auto b = fixture(true, 1);
  put(b, 149 + 12, UINT32_MAX - 5);
  writeBytes(bookPath, b);
  auto stats = BookReadingStats::load("/.crosspoint/book");
  ReadingStatsDate date;
  ASSERT_TRUE(readingStatsDateFromDayIndex(9000, date));
  auto s = span("en", 10, date, 0, 0, 0);
  ASSERT_TRUE(stats.save("/.crosspoint/book", &s));
  EXPECT_EQ(days(bookPath).at(9000).entries[2].seconds, UINT32_MAX);
}
TEST_F(LanguageStatsTest, SparseFilesBeyond32BitExtentAreRejectedBeforeNarrowing) {
  writeBytes(bookPath, fixture(true));
  std::filesystem::resize_file(storage_test::mapped(bookPath), (uint64_t(1) << 32) + 149);
  EXPECT_FALSE(BookReadingStats::load("/.crosspoint/book").persistenceWritable);
  EXPECT_FALSE(BookReadingStats{}.save("/.crosspoint/book"));
}
TEST_F(LanguageStatsTest, MetadataEditsStayDirtyUntilBothFilesPublishAndSkipSuccessfulTarget) {
  BookReadingStats b;
  GlobalReadingStats g;
  ASSERT_TRUE(saveReadingStatsWithRetry("/.crosspoint/book", b, g).complete());
  ReadingStatsEditState edits;
  edits.changed();
  b.isCompleted = true;
  g.completedBooks = 1;
  storage_test::failWritePath = "global_stats";
  EXPECT_FALSE(edits.persist("/.crosspoint/book", b, g));
  EXPECT_TRUE(edits.dirty);
  EXPECT_TRUE(edits.failed);
  ReadingStatsEditState changedAgain = edits;
  changedAgain.changed();
  EXPECT_FALSE(changedAgain.failed);
  EXPECT_TRUE(changedAgain.dirty);
  EXPECT_TRUE(edits.saved.book);
  EXPECT_FALSE(edits.saved.global);
  EXPECT_TRUE(BookReadingStats::load("/.crosspoint/book").isCompleted);
  const int bookOpens = storage_test::bookWriteOpens;
  storage_test::failWritePath.clear();
  EXPECT_TRUE(edits.persist("/.crosspoint/book", b, g));
  EXPECT_FALSE(edits.dirty);
  EXPECT_FALSE(edits.failed);
  EXPECT_EQ(storage_test::bookWriteOpens, bookOpens);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 1u);
}

TEST_F(LanguageStatsTest, BackupRejectsLargeExtentAppearingBetweenValidationAndCopy) {
  writeBytes(globalPath, fixture(false));
  storage_test::emulate32BitSize = true;
  storage_test::growOnReadOpenCall = storage_test::openReadCalls + 2;
  EXPECT_FALSE(backupGlobalStats(true, nullptr, 0));
  storage_test::emulate32BitSize = false;
  storage_test::growOnReadOpenCall = 0;
}

#include "activities/BackgroundSuspension.h"
TEST_F(LanguageStatsTest, SuspensionFailureCancelsStatsButKeepsBackgroundDrainRetryable) {
  struct Lock {};
  struct Owner {
    bool cancel = false, requested = false;
    void requestBackgroundCancellation() { requested = true; }
    bool prepareToSuspend() {
      EXPECT_TRUE(requested);
      return false;
    }
    bool cancelSuspensionOnFailure() const { return cancel; }
  } owner;
  EXPECT_FALSE(prepareBackgroundSuspension<Lock>(&owner));
  EXPECT_TRUE(retryBackgroundSuspensionAfterFailure(&owner));
  owner.cancel = true;
  EXPECT_FALSE(prepareBackgroundSuspension<Lock>(&owner));
  EXPECT_FALSE(retryBackgroundSuspensionAfterFailure(&owner));
}
