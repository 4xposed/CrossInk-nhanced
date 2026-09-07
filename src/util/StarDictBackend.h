#pragma once

#include <atomic>
#include <memory>

#include "DictHtmlRenderer.h"
#include "Dictionary.h"
#include "DictionaryEngineTypes.h"
#include "DictionaryScanIdentity.h"

class StarDictBackend {
 public:
  DictionaryStatus open(const char* bookCachePath);
  DictionaryStatus probe(const DictionaryQuery& query, DictionaryProbeResult& out);
  DictionaryStatus lookup(const DictionaryQuery& query, DictionaryResult& out);
  DictionaryStatus suggest(std::string_view word, DictionarySuggestions& out);
  DictionaryStatus streamDefinition(DictionaryDefinitionMode mode, DictionaryDefinitionSink sink);
  DictionaryCapabilities capabilities() const;
  uint64_t signature() const;
  DictionaryScanIdentityStatus beginScanIdentity(DictionaryScanIdentityState& state);
  DictionaryScanIdentityStatus stepScanIdentity(DictionaryScanIdentityState& state, size_t byteBudget);
  bool resumeScanIdentity(DictionaryScanIdentityState& state);
  void cancel();
  void close();
  bool cancelled() const;
#ifdef CROSSINK_DICT_TESTING
  bool rendererAllocatedForTesting() const { return htmlRenderer_ != nullptr; }
#endif

 private:
  static constexpr size_t kPlainBufferBytes = 512;

  DictionaryStatus normalizeQuery(const DictionaryQuery& query, std::string_view& normalized);
  DictionaryStatus locate(const DictionaryQuery& query, DictLocation& location, bool& transformed, bool& alternate,
                          std::string_view& normalized);
  static bool shouldCancel(void* context);
  bool hashFileSamples(const char* path);
  bool scanFileDescriptor(unsigned ordinal, uint64_t& size, bool& present);
  const char* scanFilePath(unsigned ordinal);
  DictionaryOwnedText resolvedPath_;
  DictionaryScanIdentityStatus identityRouteStatus_ = DictionaryScanIdentityStatus::Unavailable;
  DictionaryStatus streamPlain(DictionaryDefinitionSink sink);

  const char* bookCachePath_ = nullptr;
  DictInfo info_{};
  DictDefinitionSlice activeSlice_{};
  std::unique_ptr<char[]> plainBuffer_;
  std::unique_ptr<DictHtmlRenderer> htmlRenderer_;
  DictionaryCapabilities capabilities_{};
  uint64_t signature_ = 0;
  bool open_ = false;
  std::atomic_bool cancelled_{false};
};
