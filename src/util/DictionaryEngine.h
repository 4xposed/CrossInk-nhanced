#pragma once

#include <atomic>

#include "DictionaryEngineTypes.h"
#include "DictionaryScanIdentity.h"
#include "JapaneseDictionaryBackend.h"
#include "StarDictBackend.h"

struct DictionaryOpenRequest {
  std::string_view bookLanguage;
  const char* bookCachePath = nullptr;
};

#ifdef CROSSINK_DICT_TESTING
struct DictionaryBackendFunctions {
  void* context = nullptr;
  DictionaryStatus (*open)(void*, const DictionaryOpenRequest&) = nullptr;
  DictionaryStatus (*probe)(void*, const DictionaryQuery&, DictionaryProbeResult&) = nullptr;
  DictionaryStatus (*lookup)(void*, const DictionaryQuery&, DictionaryResult&) = nullptr;
  bool (*bookReading)(void*, std::string_view, DictionaryOwnedText&) = nullptr;
  DictionaryStatus (*suggest)(void*, std::string_view, DictionarySuggestions&) = nullptr;
  DictionaryStatus (*streamDefinition)(void*, DictionaryDefinitionMode, DictionaryDefinitionSink) = nullptr;
  DictionaryCapabilities (*capabilities)(void*) = nullptr;
  uint64_t (*signature)(void*) = nullptr;
  void (*cancel)(void*) = nullptr;
  void (*close)(void*) = nullptr;
};
#endif

class DictionaryEngine {
 public:
  DictionaryEngine() = default;
  ~DictionaryEngine() { close(); }
#ifdef CROSSINK_DICT_TESTING
  DictionaryEngine(DictionaryBackendFunctions japanese, DictionaryBackendFunctions starDict)
      : japaneseFunctions_(japanese), starDictFunctions_(starDict), useTestFunctions_(true) {}
  void setGenerationCounterForTesting(uint32_t value) { generationCounter_ = value; }
#endif

  DictionaryStatus open(const DictionaryOpenRequest& request);
  // StarDict-only UI switching stays behind the engine boundary. The override
  // is session-scoped and is cleared by close(); Japanese sessions never call
  // this operation because their sources are one automatic combined backend.
  DictionaryStatus openStarDictOverride(const DictionaryOpenRequest& request, const char* basePath);
  DictionaryStatus probe(const DictionaryQuery& query, DictionaryProbeResult& out);
  DictionaryStatus lookup(const DictionaryQuery& query, DictionaryResult& out);
  DictionaryStatus suggest(std::string_view word, DictionarySuggestions& out);
  DictionaryStatus streamDefinition(DictionaryDefinitionHandle handle, DictionaryDefinitionMode mode,
                                    DictionaryDefinitionSink sink);
  bool bookReading(std::string_view surface, DictionaryOwnedText& out);
  DictionaryBackendKind backendKind() const;
  DictionaryCapabilities capabilities() const;
  uint64_t signature() const;
  DictionaryScanIdentityStatus beginScanIdentity(DictionaryScanIdentityState& state);
  DictionaryScanIdentityStatus stepScanIdentity(DictionaryScanIdentityState& state, size_t byteBudget);
  bool resumeScanIdentity(DictionaryScanIdentityState& state);
  void cancel();
  void close();

 private:
  static bool isJapanese(std::string_view language);
  uint32_t nextEpoch();
  uint32_t nextGeneration();
  DictionaryStatus openBackend(DictionaryBackendKind kind, const DictionaryOpenRequest& request);
  DictionaryStatus probeBackend(const DictionaryQuery& query, DictionaryProbeResult& out);
  DictionaryStatus lookupBackend(const DictionaryQuery& query, DictionaryResult& out);
  DictionaryStatus suggestBackend(std::string_view word, DictionarySuggestions& out);
  DictionaryStatus streamBackend(DictionaryDefinitionMode mode, DictionaryDefinitionSink sink);
  bool bookReadingBackend(std::string_view surface, DictionaryOwnedText& out);
  void cancelBackend(DictionaryBackendKind kind);
  void closeBackend(DictionaryBackendKind kind);

#ifdef CROSSINK_DICT_TESTING
  DictionaryBackendFunctions& functions(DictionaryBackendKind kind);
  const DictionaryBackendFunctions& functions(DictionaryBackendKind kind) const;
  DictionaryBackendFunctions japaneseFunctions_{};
  DictionaryBackendFunctions starDictFunctions_{};
  bool useTestFunctions_ = false;
#endif
  JapaneseDictionaryBackend japaneseBackend_;
  StarDictBackend starDictBackend_;
  DictionaryBackendKind backendKind_ = DictionaryBackendKind::StarDict;
  uint32_t generationCounter_ = 0;
  uint32_t epochCounter_ = 0;
  uint32_t currentEpoch_ = 0;
  std::atomic_uint32_t activeGeneration_{0};
  std::atomic_uint32_t activeEpoch_{0};
  std::atomic_uint32_t cancellationEpoch_{0};
  std::atomic_bool opened_{false};
  std::atomic_bool cancelled_{false};
  bool japaneseOpened_ = false;
  bool starDictOpened_ = false;
};
