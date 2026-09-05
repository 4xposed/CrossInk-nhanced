#pragma once

#include <atomic>
#include <memory>

#include "DictionaryEngineTypes.h"
#include "WordLookup.h"

class JapaneseDictionaryBackend {
 public:
  DictionaryStatus open(const char* bookCachePath = nullptr);
  DictionaryStatus probe(const DictionaryQuery& query, DictionaryProbeResult& out);
  DictionaryStatus lookup(const DictionaryQuery& query, DictionaryResult& out);
  DictionaryStatus suggest(std::string_view word, DictionarySuggestions& out);
  DictionaryStatus streamDefinition(DictionaryDefinitionMode mode, DictionaryDefinitionSink sink);
  bool bookReading(std::string_view surface, DictionaryOwnedText& out);
  DictionaryCapabilities capabilities() const;
  uint64_t signature() const;
  void cancel();
  void close();

 private:
  static DictionaryStatus mapStatus(JapaneseDictStatus status);

  DictIndex index_;
  WordLookup lookup_{index_};
  DictEntry activeEntry_;
  DictionaryOwnedText syntheticNameDefinition_;
  DictionaryCapabilities capabilities_{};
  std::unique_ptr<char[]> bookCachePath_;
  size_t bookCachePathLength_ = 0;
  bool open_ = false;
  std::atomic_bool cancelled_{false};
};
