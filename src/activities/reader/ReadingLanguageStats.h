#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ReadingStatsUtils.h"

class HalFile;
struct ReadingLanguageEntry {
  char tag[4]{};
  uint32_t seconds = 0;
};
struct ReadingLanguageTotals {
  ReadingLanguageEntry entries[8] = {{{'u', 'n', 'd', 0}, 0}, {{'m', 'u', 'l', 0}, 0}};
};
static_assert(sizeof(ReadingLanguageEntry) == 8 && sizeof(ReadingLanguageTotals) == 64);
struct ReadingLanguageSpan {
  ReadingStatsDateTime localStart;
  uint32_t seconds = 0;
  char normalizedTag[4] = "und";
};

bool normalizeReadingLanguage(std::string_view input, char (&out)[4]);
uint32_t saturateReadingLanguageSeconds(uint32_t a, uint32_t b);
uint8_t addReadingLanguageSeconds(ReadingLanguageTotals&, const char* normalizedTag, uint32_t seconds);
bool validateReadingLanguageTotals(const ReadingLanguageTotals&);
void seedUnknownReadingLanguage(ReadingLanguageTotals&, uint32_t totalSeconds);
void selectReadingLanguageTags(ReadingLanguageTotals& selected, const ReadingLanguageTotals& source);
void mergeSelectedReadingLanguageTotals(ReadingLanguageTotals& target, const ReadingLanguageTotals& source);
bool readReadingLanguageTotals(HalFile&, ReadingLanguageTotals&);
bool writeReadingLanguageTotals(HalFile&, const ReadingLanguageTotals&);
bool decodeReadingLanguageTotals(const uint8_t* bytes, ReadingLanguageTotals&);

enum class ReadingStatsParseMode : uint8_t { Local, SyncedSummary };
struct ReadingLanguageFileInfo {
  size_t size = 0;
  uint16_t summarySize = 0;
  uint16_t rows = 0;
  uint32_t anchor = 0;
  uint8_t version = 0;
  bool hasAppendix = false;
};
// Validates exact lengths, the fixed language summary and every local day row.
bool inspectReadingLanguageFile(HalFile&, bool book, ReadingStatsParseMode, ReadingLanguageFileInfo&);
bool validateGlobalReadingStatsSummary(const uint8_t* data, size_t size);
bool readGlobalReadingStatsSummary(const char* path, uint8_t* out, uint8_t& size);

// Prefix callback writes only existing fields (73 book / 159 global bytes).
using ReadingStatsPrefixWriter = bool (*)(HalFile&, const void*);
// oldPath is a validated recovery/migration source, or null for a fresh file.
// keepBackup preserves backupPath (e.g. corrupt primary recovery or reset).
bool publishReadingLanguageFile(const char* path, const char* backupPath, const char* oldPath, bool book,
                                const ReadingLanguageTotals&, const ReadingLanguageSpan*, ReadingStatsPrefixWriter,
                                const void* stats, bool keepBackup = false);
bool visitLocalReadingLanguageDays(const char* path, bool (*visitor)(void*, uint32_t, const ReadingLanguageTotals&),
                                   void* context);
