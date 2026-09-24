#include "DictionaryRegistry.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "DictIndex.h"
#include "Dictionary.h"

static constexpr size_t DICT_FILENAME_BUFFER_SIZE = 256;  // FAT LFN max (255) plus terminator.

// Candidate StarDict roots, scanned in priority order. Equivalent relative
// folders in a later root are ignored, but their full paths remain valid saved
// fallbacks.
static constexpr const char* DICT_ROOT_CANDIDATES[] = {
    "/.dictionaries",
    "/dictionaries",
};

namespace {
char asciiLower(const char value) { return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value; }

struct FolderScan {
  std::string stem;
  bool ambiguous = false;
  bool foundDict = false;
  int ifoCount = 0;
};

enum class ScanStatus : uint8_t { Success, Unavailable, AllocationFailed };

struct JapanesePathPair {
  const char* index;
  const char* data;
};

// Keep this order aligned with DictIndex: preferred CrossInk locations first,
// then the legacy Matcha /dict location and filename.
static constexpr JapanesePathPair VOCAB_PATHS[] = {
    {"/dictionaries/jp/vocab.idx", "/dictionaries/jp/vocab.dat"},
    {"/dictionaries/jp/jmdict.idx", "/dictionaries/jp/jmdict.dat"},
    {"/dict/vocab.idx", "/dict/vocab.dat"},
    {"/dict/jmdict.idx", "/dict/jmdict.dat"},
};
static constexpr JapanesePathPair GRAMMAR_PATHS[] = {
    {"/dictionaries/jp/grammar.idx", "/dictionaries/jp/grammar.dat"},
    {"/dict/grammar.idx", "/dict/grammar.dat"},
};
static constexpr JapanesePathPair NAMES_PATHS[] = {
    {"/dictionaries/jp/names.idx", "/dictionaries/jp/names.dat"},
    {"/dictionaries/jp/jmnedict.idx", "/dictionaries/jp/jmnedict.dat"},
    {"/dict/names.idx", "/dict/names.dat"},
    {"/dict/jmnedict.idx", "/dict/jmnedict.dat"},
};

bool isSafePathSegment(const std::string_view segment) {
  return !segment.empty() && segment != "." && segment != ".." && segment.find('/') == std::string_view::npos &&
         segment.find('\\') == std::string_view::npos;
}

bool hasExtension(const char* name, const size_t length, const char* extension, const size_t extensionLength) {
  return length > extensionLength && strcmp(name + length - extensionLength, extension) == 0;
}

ScanStatus scanFolder(const std::string& path, FolderScan& result, char* name, const size_t nameSize) {
  auto directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    const bool allocationFailed = directory.allocationFailed();
    LOG_ERR("DREG", "Could not open dictionary folder: %s", path.c_str());
    if (directory) directory.close();
    return allocationFailed ? ScanStatus::AllocationFailed : ScanStatus::Unavailable;
  }

  result = FolderScan{};
  directory.rewindDirectory();
  while (true) {
    auto entry = directory.openNextFile();
    if (!entry) break;
    entry.getName(name, nameSize);
    const bool isDirectory = entry.isDirectory();
    entry.close();

    if (!isSafePathSegment(name)) {
      LOG_DBG("DREG", "Skipping unsafe dictionary path segment: %s", name);
      continue;
    }
    if (strncmp(name, "._", 2) == 0 || strcasecmp(name, ".DS_Store") == 0) continue;
    if (isDirectory) continue;

    const size_t length = strlen(name);
    const bool isIdx = hasExtension(name, length, ".idx", 4);
    const bool isIfo = hasExtension(name, length, ".ifo", 4);
    const bool isDict = hasExtension(name, length, ".dict", 5);
    if (isIfo) ++result.ifoCount;
    if (isDict) result.foundDict = true;
    if (!isIdx) continue;
    if (!result.stem.empty()) {
      result.ambiguous = true;
      continue;
    }
    result.stem.assign(name, length - 4);
  }
  if (directory.allocationFailed()) {
    LOG_ERR("DREG", "Could not enumerate dictionary folder: %s", path.c_str());
    directory.close();
    return ScanStatus::AllocationFailed;
  }
  directory.close();
  if (result.ifoCount > 1) result.ambiguous = true;
  return ScanStatus::Success;
}

template <size_t Count>
bool firstJapanesePairExists(const JapanesePathPair (&paths)[Count]) {
  return std::any_of(std::begin(paths), std::end(paths),
                     [](const auto& path) { return Storage.exists(path.index) && Storage.exists(path.data); });
}

bool firstJapaneseVocabularyPairValid() {
  for (const auto& path : VOCAB_PATHS) {
    // DictIndex selects the first existing index and does not skip an invalid
    // preferred pair in favor of a later one.
    if (!Storage.exists(path.index)) continue;
    if (!Storage.exists(path.data)) return false;

    HalFile index;
    HalFile data;
    if (!Storage.openFileForRead("DREG", path.index, index)) return false;
    if (!Storage.openFileForRead("DREG", path.data, data)) {
      index.close();
      return false;
    }
    const size_t indexSize = index.fileSize();
    bool valid = indexSize % sizeof(DictIndexRecord) == 0;
    if (valid && indexSize != 0) {
      uint8_t sample = 0;
      valid = index.seekSet(0) && index.read(&sample, 1) == 1 && index.seekSet(indexSize - 1) &&
              index.read(&sample, 1) == 1;
    }
    data.close();
    index.close();
    return valid;
  }
  return false;
}

bool isJapaneseBookLanguage(const std::string_view language) {
  if (language.size() < 2) return false;
  return asciiLower(language[0]) == 'j' && asciiLower(language[1]) == 'a' &&
         (language.size() == 2 || language[2] == '-' || language[2] == '_');
}

bool caseInsensitiveLess(const std::string& left, const std::string& right) {
  const size_t shared = std::min(left.size(), right.size());
  for (size_t index = 0; index < shared; ++index) {
    const char leftChar = asciiLower(left[index]);
    const char rightChar = asciiLower(right[index]);
    if (leftChar != rightChar) return leftChar < rightChar;
  }
  if (left.size() != right.size()) return left.size() < right.size();
  return left < right;
}

bool caseInsensitiveEqual(const std::string& left, const std::string& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const char a, const char b) { return asciiLower(a) == asciiLower(b); });
}

bool caseInsensitiveEqual(const std::string_view left, const std::string_view right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const char a, const char b) { return asciiLower(a) == asciiLower(b); });
}

void appendIfUnique(std::vector<DictionaryEntry>& entries, std::vector<std::pair<std::string, std::string>>& aliases,
                    DictionaryEntry entry, const size_t higherPriorityEntryCount) {
  const auto collision = std::find_if(entries.begin(), entries.end(), [&](const auto& existing) {
    return caseInsensitiveEqual(existing.name, entry.name);
  });
  if (collision != entries.end()) {
    const size_t collisionIndex = static_cast<size_t>(collision - entries.begin());
    if (collisionIndex < higherPriorityEntryCount && caseInsensitiveEqual(collision->stem, entry.stem)) {
      const auto existingAlias = std::find_if(aliases.begin(), aliases.end(), [&](const auto& alias) {
        return caseInsensitiveEqual(alias.second, collision->basePath);
      });
      if (existingAlias == aliases.end()) aliases.emplace_back(entry.basePath, collision->basePath);
    }
    LOG_DBG("DREG", "Skipping duplicate dictionary folder: %s", entry.basePath.c_str());
    return;
  }
  entries.push_back(std::move(entry));
}

bool normalizeLanguage(std::string_view language, char (&normalized)[3]) {
  if (language.find('/') != std::string_view::npos || language.find('\\') != std::string_view::npos ||
      language.find("..") != std::string_view::npos || language.size() < 2) {
    return false;
  }
  for (size_t index = 0; index < 2; ++index) {
    const char value = asciiLower(language[index]);
    if (value < 'a' || value > 'z') return false;
    normalized[index] = value;
  }
  normalized[2] = '\0';
  if (normalized[0] == 'j' && normalized[1] == 'a') normalized[1] = 'p';
  return true;
}

ScanStatus scanRoot(const std::string& root, std::vector<DictionaryEntry>& entries,
                    std::vector<std::pair<std::string, std::string>>& aliases, const size_t higherPriorityEntryCount,
                    char* name, const size_t nameSize) {
  auto rootDir = Storage.open(root.c_str());
  if (!rootDir || !rootDir.isDirectory()) {
    const bool allocationFailed = rootDir.allocationFailed();
    LOG_ERR("DREG", "Could not open dictionary directory: %s", root.c_str());
    if (rootDir) rootDir.close();
    return allocationFailed ? ScanStatus::AllocationFailed : ScanStatus::Unavailable;
  }

  rootDir.rewindDirectory();
  while (true) {
    auto entry = rootDir.openNextFile();
    if (!entry) break;
    entry.getName(name, nameSize);

    if (!entry.isDirectory() || name[0] == '.') {
      entry.close();
      continue;
    }

    if (!isSafePathSegment(name)) {
      entry.close();
      LOG_DBG("DREG", "Skipping unsafe dictionary path segment: %s", name);
      continue;
    }

    const std::string languageName = name;
    const std::string subPath = root + "/" + languageName;
    entry.close();

    FolderScan languageScan;
    const ScanStatus languageStatus = scanFolder(subPath, languageScan, name, nameSize);
    if (languageStatus == ScanStatus::AllocationFailed) {
      rootDir.close();
      return languageStatus;
    }
    const bool isJapaneseFolder = strcasecmp(languageName.c_str(), "jp") == 0;
    if (languageStatus == ScanStatus::Success && !isJapaneseFolder && !languageScan.ambiguous &&
        !languageScan.stem.empty() && languageScan.foundDict) {
      DictionaryEntry dictionary;
      dictionary.name = languageName;
      dictionary.stem = languageScan.stem;
      dictionary.basePath = root + "/" + dictionary.name + "/" + dictionary.stem;
      LOG_DBG("DREG", "Found dictionary: %s/%s", languageName.c_str(), languageScan.stem.c_str());
      appendIfUnique(entries, aliases, std::move(dictionary), higherPriorityEntryCount);
    } else if (languageStatus == ScanStatus::Success && !isJapaneseFolder && languageScan.ambiguous) {
      LOG_DBG("DREG", "Skipping %s: multiple .idx or .ifo files found", languageName.c_str());
    }

    auto languageDirectory = Storage.open(subPath.c_str());
    if (!languageDirectory || !languageDirectory.isDirectory()) {
      const bool allocationFailed = languageDirectory.allocationFailed();
      LOG_ERR("DREG", "Could not reopen dictionary folder: %s", subPath.c_str());
      if (languageDirectory) languageDirectory.close();
      if (allocationFailed) {
        rootDir.close();
        return ScanStatus::AllocationFailed;
      }
      continue;
    }
    languageDirectory.rewindDirectory();
    while (true) {
      auto nested = languageDirectory.openNextFile();
      if (!nested) break;
      nested.getName(name, nameSize);
      const bool isDirectory = nested.isDirectory();
      nested.close();
      if (!isDirectory || !isSafePathSegment(name) || name[0] == '.') continue;
      const std::string nestedName = name;
      const std::string relativePath = languageName + "/" + nestedName;
      const std::string nestedPath = root + "/" + relativePath;
      FolderScan nestedScan;
      const ScanStatus nestedStatus = scanFolder(nestedPath, nestedScan, name, nameSize);
      if (nestedStatus == ScanStatus::AllocationFailed) {
        languageDirectory.close();
        rootDir.close();
        return nestedStatus;
      }
      if (nestedStatus != ScanStatus::Success) continue;
      if (nestedScan.ambiguous) {
        LOG_DBG("DREG", "Skipping %s: multiple .idx or .ifo files found", relativePath.c_str());
        continue;
      }
      if (nestedScan.stem.empty() || !nestedScan.foundDict) continue;
      DictionaryEntry dictionary;
      dictionary.name = relativePath;
      dictionary.stem = nestedScan.stem;
      dictionary.basePath = nestedPath + "/" + dictionary.stem;
      LOG_DBG("DREG", "Found dictionary: %s/%s", relativePath.c_str(), nestedScan.stem.c_str());
      appendIfUnique(entries, aliases, std::move(dictionary), higherPriorityEntryCount);
    }
    if (languageDirectory.allocationFailed()) {
      LOG_ERR("DREG", "Could not enumerate dictionary folder: %s", subPath.c_str());
      languageDirectory.close();
      rootDir.close();
      return ScanStatus::AllocationFailed;
    }
    languageDirectory.close();
  }
  if (rootDir.allocationFailed()) {
    LOG_ERR("DREG", "Could not enumerate dictionary directory: %s", root.c_str());
    rootDir.close();
    return ScanStatus::AllocationFailed;
  }
  rootDir.close();
  return ScanStatus::Success;
}
}  // namespace

bool DictionaryRegistry::discover(const bool autoSelectDefault) {
  clear();
  std::vector<DictionaryEntry> discoveredEntries;
  discoveredEntries.reserve(16);
  std::vector<std::pair<std::string, std::string>> discoveredAliases;
  std::string firstRoot;
  char name[DICT_FILENAME_BUFFER_SIZE];
  for (size_t rootIndex = 0; rootIndex < std::size(DICT_ROOT_CANDIDATES); ++rootIndex) {
    const auto* candidate = DICT_ROOT_CANDIDATES[rootIndex];
    char resolvedRoot[32];
    if (!FsHelpers::resolveRootDirectoryIgnoreCase(candidate, resolvedRoot, sizeof(resolvedRoot))) continue;
    if (firstRoot.empty()) firstRoot = resolvedRoot;
    const size_t higherPriorityEntryCount = discoveredEntries.size();
    if (scanRoot(resolvedRoot, discoveredEntries, discoveredAliases, higherPriorityEntryCount, name, sizeof(name)) ==
        ScanStatus::AllocationFailed) {
      LOG_ERR("DREG", "Dictionary discovery aborted after allocation failure");
      return false;
    }
  }

  // Sort alphabetically by folder name (case-insensitive — matches FileBrowserActivity).
  std::sort(discoveredEntries.begin(), discoveredEntries.end(),
            [](const DictionaryEntry& a, const DictionaryEntry& b) { return caseInsensitiveLess(a.name, b.name); });

  entries_.swap(discoveredEntries);
  aliases_.swap(discoveredAliases);
  root_ = std::move(firstRoot);
  japaneseBundle_.vocabulary = firstJapanesePairExists(VOCAB_PATHS);
  japaneseBundle_.names = firstJapanesePairExists(NAMES_PATHS);
  japaneseBundle_.grammar = firstJapanesePairExists(GRAMMAR_PATHS);
  japaneseVocabularyValid_ = firstJapaneseVocabularyPairValid();

  LOG_DBG("DREG", "Discovery complete: %d dictionaries across supported roots", static_cast<int>(entries_.size()));

  if (autoSelectDefault) maybeAutoSelectDefaultDictionary();

  return !entries_.empty() || japaneseBundle_.vocabulary;
}

void DictionaryRegistry::clear() {
  std::vector<DictionaryEntry>().swap(entries_);
  std::vector<std::pair<std::string, std::string>>().swap(aliases_);
  std::string().swap(root_);
  japaneseBundle_ = {};
  japaneseVocabularyValid_ = false;
}

int DictionaryRegistry::indexOf(const std::string& basePath) const {
  if (basePath.empty()) return -1;
  for (size_t i = 0; i < entries_.size(); i++) {
    if (entries_[i].basePath == basePath) return static_cast<int>(i);
  }
  return -1;
}

int DictionaryRegistry::indexOfExactOrEquivalent(const std::string_view basePath) const {
  if (basePath.empty()) return -1;
  for (size_t index = 0; index < entries_.size(); ++index) {
    if (caseInsensitiveEqual(basePath, entries_[index].basePath)) return static_cast<int>(index);
  }

  for (const auto& alias : aliases_) {
    if (!caseInsensitiveEqual(basePath, alias.first)) continue;
    for (size_t index = 0; index < entries_.size(); ++index) {
      if (caseInsensitiveEqual(alias.second, entries_[index].basePath)) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }
  return -1;
}

const DictionaryEntry* DictionaryRegistry::firstForLanguage(const std::string_view language) const {
  char normalized[3];
  if (!normalizeLanguage(language, normalized)) return nullptr;
  for (const auto& entry : entries_) {
    const size_t separator = entry.name.find('/');
    const std::string_view folder(entry.name.data(), separator == std::string::npos ? entry.name.size() : separator);
    if (folder.size() == 2 && asciiLower(folder[0]) == normalized[0] && asciiLower(folder[1]) == normalized[1]) {
      return &entry;
    }
  }
  return nullptr;
}

bool DictionaryRegistry::resolveEffectiveStarDict(const std::string_view language, const char* bookCachePath,
                                                  std::string& basePathOut) const {
  if (const auto* languageEntry = firstForLanguage(language)) {
    basePathOut = languageEntry->basePath;
    return true;
  }
  basePathOut = Dictionary::readConfiguredDictPath(bookCachePath);
  const int configuredIndex = indexOfExactOrEquivalent(basePathOut);
  if (configuredIndex >= 0) basePathOut = entries_[static_cast<size_t>(configuredIndex)].basePath;
  return !basePathOut.empty();
}

bool DictionaryRegistry::lookupAvailable(const std::string_view language, const char* bookCachePath) const {
  std::string starDictPath;
  return resolveLookupRoute(language, bookCachePath, starDictPath);
}

bool DictionaryRegistry::resolveLookupRoute(const std::string_view language, const char* bookCachePath,
                                            std::string& starDictPathOut) const {
  starDictPathOut.clear();
  if (isJapaneseBookLanguage(language) && japaneseVocabularyValid_) return true;

  if (!resolveEffectiveStarDict(language, bookCachePath, starDictPathOut) || starDictPathOut.empty()) return false;
  const DictPaths paths(starDictPathOut);
  if (Storage.exists(paths.idx().c_str()) && Storage.exists(paths.dict().c_str())) return true;
  starDictPathOut.clear();
  return false;
}

void DictionaryRegistry::maybeAutoSelectDefaultDictionary() const {
  if (entries_.empty()) return;

  const std::string activePath = Dictionary::readConfiguredDictPath();
  if (activePath == entries_[0].basePath) return;
  if (activePath.empty() && Dictionary::hasGlobalDictPathFile()) return;
  if (!activePath.empty() && indexOfExactOrEquivalent(activePath) >= 0) return;
  if (!activePath.empty()) {
    const DictPaths paths(activePath);
    if (Storage.exists(paths.idx().c_str()) && Storage.exists(paths.dict().c_str())) return;
  }

  LOG_INF("DREG", "Auto-selecting first discovered dictionary: %s", entries_[0].basePath.c_str());
  Dictionary::saveGlobalDictPath(entries_[0].basePath.c_str());
}

bool resolveTransientDictionaryLookupRoute(const std::string_view language, const char* bookCachePath,
                                           std::string& starDictPathOut) {
  // A valid Japanese bundle already wins routing. Avoid enumerating unrelated
  // StarDict folders (and allocating their catalog) on every word lookup.
  if (isJapaneseBookLanguage(language) && firstJapaneseVocabularyPairValid()) {
    starDictPathOut.clear();
    return true;
  }
  DictionaryRegistry registry;
  registry.discover(/*autoSelectDefault=*/false);
  const bool available = registry.resolveLookupRoute(language, bookCachePath, starDictPathOut);
  registry.clear();
  return available;
}
