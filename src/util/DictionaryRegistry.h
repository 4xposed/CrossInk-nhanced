#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One installed StarDict dictionary discovered on the SD card.
struct DictionaryEntry {
  std::string name;      // root-relative folder name, e.g. "en/dict-en-en"
  std::string stem;      // file stem (".idx"/".ifo" base), e.g. "dict-data"
  std::string basePath;  // <root>/<name>/<stem>, e.g. "/.dictionaries/en/dict-en-en/dict-data"
};

struct JapaneseDictionaryBundle {
  bool vocabulary = false;
  bool names = false;
  bool grammar = false;
};

// Discovers installed dictionaries on the SD card. Mirrors SdCardFontRegistry:
// discover() scans the card and populates entries_; the settings UI enumerates
// them on both device and web. The persistent selection is not stored here — it
// lives in dictionary.bin (see Dictionary::readConfiguredDictPath/saveGlobalDictPath).
class DictionaryRegistry {
 public:
  // Scan the SD card, populate entries_ (sorted by folder name). Returns true if any found.
  bool discover(bool autoSelectDefault = true);
  void clear();

  const std::vector<DictionaryEntry>& getEntries() const { return entries_; }
  int count() const { return static_cast<int>(entries_.size()); }
  // First available StarDict root, retained for compatibility. Entries may
  // belong to either supported root; use DictionaryEntry::basePath for access.
  const std::string& root() const { return root_; }
  JapaneseDictionaryBundle japaneseBundle() const { return japaneseBundle_; }

  // Returns the first sorted StarDict dictionary under the normalized two-letter
  // language folder. Japanese language tags map from "ja" to the on-disk "jp" folder.
  const DictionaryEntry* firstForLanguage(std::string_view language) const;

  // Prefer a language-folder dictionary, then preserve the current per-book/global
  // StarDict persistence contract as the fallback.
  bool resolveEffectiveStarDict(std::string_view language, const char* bookCachePath, std::string& basePathOut) const;

  // Read-only entry-point availability. Japanese mirrors the engine's first
  // vocabulary-pair validation and then falls back to the effective StarDict.
  // All transient validation handles are closed before this returns.
  bool lookupAvailable(std::string_view language, const char* bookCachePath) const;

  // Resolves the route that EpubReaderWordLookupActivity must open. An empty
  // StarDict path means the automatic Japanese backend; a non-empty path is
  // the exact language-first/per-book/global StarDict override.
  bool resolveLookupRoute(std::string_view language, const char* bookCachePath, std::string& starDictPathOut) const;

  // Index of the entry whose basePath == path, or -1 if not found / path empty.
  int indexOf(const std::string& basePath) const;

  // Index of an exact entry path or a verified path discarded as a lower-priority
  // collision during the latest scan. Paths inferred only from their shape never alias.
  int indexOfExactOrEquivalent(std::string_view basePath) const;

  // Mark the registry as needing a re-scan. Thread-safe (callable from the web task).
  void markDirty() { dirty_.store(true, std::memory_order_release); }

  // Re-scan if marked dirty, then clear the flag. Mirrors SdCardFontSystem::refreshIfDirty().
  void refreshIfDirty() {
    if (dirty_.exchange(false, std::memory_order_acquire)) discover();
  }

 private:
  void maybeAutoSelectDefaultDictionary() const;

  std::vector<DictionaryEntry> entries_;                      // sorted alphabetically by name
  std::vector<std::pair<std::string, std::string>> aliases_;  // discarded path -> retained path
  std::string root_;                                          // first available root, compatibility only
  JapaneseDictionaryBundle japaneseBundle_;
  bool japaneseVocabularyValid_ = false;
  std::atomic<bool> dirty_{false};
};

// Resolve one reader lookup without mutating or clearing the shared settings
// catalog. The transient registry is released before this function returns,
// so callers may open the selected engine without retaining a second catalog.
bool resolveTransientDictionaryLookupRoute(std::string_view language, const char* bookCachePath,
                                           std::string& starDictPathOut);

// Global dictionary registry instance (defined in main.cpp).
extern DictionaryRegistry dictionaryRegistry;
