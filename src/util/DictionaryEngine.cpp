#include "DictionaryEngine.h"

#include <Logging.h>

#include <utility>

#include "Dictionary.h"

namespace {
constexpr size_t kMaxDictionaryOverridePathBytes = 512;
}

bool DictionaryEngine::isJapanese(const std::string_view language) {
  if (language.size() < 2) return false;
  const char first = language[0] >= 'A' && language[0] <= 'Z' ? language[0] + ('a' - 'A') : language[0];
  const char second = language[1] >= 'A' && language[1] <= 'Z' ? language[1] + ('a' - 'A') : language[1];
  return first == 'j' && second == 'a' && (language.size() == 2 || language[2] == '-' || language[2] == '_');
}

#ifdef CROSSINK_DICT_TESTING
DictionaryBackendFunctions& DictionaryEngine::functions(const DictionaryBackendKind kind) {
  return kind == DictionaryBackendKind::Japanese ? japaneseFunctions_ : starDictFunctions_;
}

const DictionaryBackendFunctions& DictionaryEngine::functions(const DictionaryBackendKind kind) const {
  return kind == DictionaryBackendKind::Japanese ? japaneseFunctions_ : starDictFunctions_;
}
#endif

DictionaryStatus DictionaryEngine::openBackend(const DictionaryBackendKind kind, const DictionaryOpenRequest& request) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(kind);
    return backend.open ? backend.open(backend.context, request) : DictionaryStatus::Unavailable;
  }
#endif
  switch (kind) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.open(request.bookCachePath);
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.open(request.bookCachePath);
  }
  return DictionaryStatus::Unavailable;
}

DictionaryStatus DictionaryEngine::open(const DictionaryOpenRequest& request) {
  close();
  currentEpoch_ = nextEpoch();
  cancelled_ = false;
  if (isJapanese(request.bookLanguage)) {
    japaneseOpened_ = true;
    DictionaryStatus status = openBackend(DictionaryBackendKind::Japanese, request);
    if (status == DictionaryStatus::Found) {
      backendKind_ = DictionaryBackendKind::Japanese;
      opened_ = true;
      return status;
    }
    if (status == DictionaryStatus::OutOfMemory || status == DictionaryStatus::Cancelled) return status;
    closeBackend(DictionaryBackendKind::Japanese);
    japaneseOpened_ = false;
    const DictionaryStatus japaneseStatus = status;
    starDictOpened_ = true;
    status = openBackend(DictionaryBackendKind::StarDict, request);
    if (status == DictionaryStatus::Found) {
      backendKind_ = DictionaryBackendKind::StarDict;
      opened_ = true;
      return status;
    }
    return status == DictionaryStatus::Unavailable ? japaneseStatus : status;
  }

  starDictOpened_ = true;
  const DictionaryStatus status = openBackend(DictionaryBackendKind::StarDict, request);
  if (status == DictionaryStatus::Found) {
    backendKind_ = DictionaryBackendKind::StarDict;
    opened_ = true;
  }
  return status;
}

DictionaryStatus DictionaryEngine::openStarDictOverride(const DictionaryOpenRequest& request, const char* basePath) {
  close();
  if (!basePath || basePath[0] == '\0') return DictionaryStatus::Unavailable;
  size_t length = 0;
  while (length <= kMaxDictionaryOverridePathBytes && basePath[length] != '\0') ++length;
  if (length > kMaxDictionaryOverridePathBytes) {
    LOG_ERR("DICT", "StarDict override path exceeds %u bytes", static_cast<unsigned>(kMaxDictionaryOverridePathBytes));
    return DictionaryStatus::ReadError;
  }
  Dictionary::setLookupDictPathOverride(basePath);
  currentEpoch_ = nextEpoch();
  cancelled_ = false;
  starDictOpened_ = true;
  const DictionaryStatus status = openBackend(DictionaryBackendKind::StarDict, request);
  if (status == DictionaryStatus::Found) {
    backendKind_ = DictionaryBackendKind::StarDict;
    opened_ = true;
    return status;
  }
  closeBackend(DictionaryBackendKind::StarDict);
  starDictOpened_ = false;
  return status;
}

DictionaryStatus DictionaryEngine::probeBackend(const DictionaryQuery& query, DictionaryProbeResult& out) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(backendKind_);
    return backend.probe ? backend.probe(backend.context, query, out) : DictionaryStatus::Unavailable;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.probe(query, out);
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.probe(query, out);
  }
  return DictionaryStatus::Unavailable;
}

DictionaryStatus DictionaryEngine::probe(const DictionaryQuery& query, DictionaryProbeResult& out) {
  if (!opened_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  DictionaryProbeResult complete;
  const DictionaryStatus status = probeBackend(query, complete);
  if (cancelled_.load()) return DictionaryStatus::Cancelled;
  complete.status = status;
  out = complete;
  return status;
}

DictionaryStatus DictionaryEngine::lookupBackend(const DictionaryQuery& query, DictionaryResult& out) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(backendKind_);
    return backend.lookup ? backend.lookup(backend.context, query, out) : DictionaryStatus::Unavailable;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.lookup(query, out);
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.lookup(query, out);
  }
  return DictionaryStatus::Unavailable;
}

uint32_t DictionaryEngine::nextEpoch() {
  ++epochCounter_;
  if (epochCounter_ == 0) ++epochCounter_;
  return epochCounter_;
}

uint32_t DictionaryEngine::nextGeneration() {
  ++generationCounter_;
  if (generationCounter_ == 0) {
    ++generationCounter_;
    currentEpoch_ = nextEpoch();
  }
  return generationCounter_;
}

DictionaryStatus DictionaryEngine::lookup(const DictionaryQuery& query, DictionaryResult& out) {
  if (!opened_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  const uint32_t cancellationEpoch = cancellationEpoch_.load(std::memory_order_acquire);

  DictionaryResult complete;
  const DictionaryStatus status = lookupBackend(query, complete);
  if (status != DictionaryStatus::Found) return status;
  if (cancelled_.load() || cancellationEpoch_.load(std::memory_order_acquire) != cancellationEpoch) {
    return DictionaryStatus::Cancelled;
  }
  complete.status = DictionaryStatus::Found;
  complete.backend = backendKind_;
  complete.definition.generation = nextGeneration();
  complete.definition.epoch = currentEpoch_;
  // Cancellation is cross-task. Recheck immediately before publishing the
  // result, then expose the matching generation with atomic release ordering.
  if (cancelled_.load() || cancellationEpoch_.load(std::memory_order_acquire) != cancellationEpoch) {
    return DictionaryStatus::Cancelled;
  }
  activeGeneration_.store(complete.definition.generation, std::memory_order_release);
  activeEpoch_.store(complete.definition.epoch, std::memory_order_release);
  // A cancel can land between the pre-publication check and the generation
  // store. The monotonically changing epoch detects that interleave and
  // retracts only this generation, without reviving a handle cancel cleared.
  if (cancelled_.load() || cancellationEpoch_.load(std::memory_order_acquire) != cancellationEpoch) {
    uint32_t published = complete.definition.generation;
    activeGeneration_.compare_exchange_strong(published, 0, std::memory_order_acq_rel);
    uint32_t publishedEpoch = complete.definition.epoch;
    activeEpoch_.compare_exchange_strong(publishedEpoch, 0, std::memory_order_acq_rel);
    return DictionaryStatus::Cancelled;
  }
  out = std::move(complete);
  return DictionaryStatus::Found;
}

DictionaryStatus DictionaryEngine::suggestBackend(const std::string_view word, DictionarySuggestions& out) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(backendKind_);
    return backend.suggest ? backend.suggest(backend.context, word, out) : DictionaryStatus::Unavailable;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.suggest(word, out);
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.suggest(word, out);
  }
  return DictionaryStatus::Unavailable;
}

DictionaryStatus DictionaryEngine::suggest(const std::string_view word, DictionarySuggestions& out) {
  if (!opened_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  DictionarySuggestions complete;
  const DictionaryStatus status = suggestBackend(word, complete);
  if (cancelled_.load()) return DictionaryStatus::Cancelled;
  if (status == DictionaryStatus::Found) out = std::move(complete);
  return status;
}

DictionaryStatus DictionaryEngine::streamBackend(const DictionaryDefinitionMode mode,
                                                 const DictionaryDefinitionSink sink) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(backendKind_);
    return backend.streamDefinition ? backend.streamDefinition(backend.context, mode, sink)
                                    : DictionaryStatus::Unavailable;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.streamDefinition(mode, sink);
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.streamDefinition(mode, sink);
  }
  return DictionaryStatus::Unavailable;
}

bool DictionaryEngine::bookReadingBackend(const std::string_view surface, DictionaryOwnedText& out) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(backendKind_);
    return backend.bookReading ? backend.bookReading(backend.context, surface, out) : false;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return false;
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.bookReading(surface, out);
  }
  return false;
}

DictionaryStatus DictionaryEngine::streamDefinition(const DictionaryDefinitionHandle handle,
                                                    const DictionaryDefinitionMode mode,
                                                    const DictionaryDefinitionSink sink) {
  if (!opened_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  if (handle.generation == 0 || handle.epoch == 0 || handle.epoch != activeEpoch_.load(std::memory_order_acquire) ||
      handle.generation != activeGeneration_.load(std::memory_order_acquire)) {
    return DictionaryStatus::Unavailable;
  }
  const DictionaryStatus status = streamBackend(mode, sink);
  return cancelled_.load() ? DictionaryStatus::Cancelled : status;
}

bool DictionaryEngine::bookReading(const std::string_view surface, DictionaryOwnedText& out) {
  DictionaryOwnedText complete;
  if (!opened_ || cancelled_ || surface.empty() || !bookReadingBackend(surface, complete) || cancelled_.load()) {
    out.reset();
    return false;
  }
  out = std::move(complete);
  return true;
}

DictionaryBackendKind DictionaryEngine::backendKind() const { return backendKind_; }

DictionaryCapabilities DictionaryEngine::capabilities() const {
  if (!opened_) return {};
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    const auto& backend = functions(backendKind_);
    return backend.capabilities ? backend.capabilities(backend.context) : DictionaryCapabilities{};
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.capabilities();
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.capabilities();
  }
  return {};
}

uint64_t DictionaryEngine::signature() const {
  if (!opened_) return 0;
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    const auto& backend = functions(backendKind_);
    return backend.signature ? backend.signature(backend.context) : 0;
  }
#endif
  switch (backendKind_) {
    case DictionaryBackendKind::StarDict:
      return starDictBackend_.signature();
    case DictionaryBackendKind::Japanese:
      return japaneseBackend_.signature();
  }
  return 0;
}

void DictionaryEngine::cancelBackend(const DictionaryBackendKind kind) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(kind);
    if (backend.cancel) backend.cancel(backend.context);
    return;
  }
#endif
  switch (kind) {
    case DictionaryBackendKind::StarDict:
      starDictBackend_.cancel();
      return;
    case DictionaryBackendKind::Japanese:
      japaneseBackend_.cancel();
      return;
  }
}

void DictionaryEngine::closeBackend(const DictionaryBackendKind kind) {
#ifdef CROSSINK_DICT_TESTING
  if (useTestFunctions_) {
    auto& backend = functions(kind);
    if (backend.close) backend.close(backend.context);
    return;
  }
#endif
  switch (kind) {
    case DictionaryBackendKind::StarDict:
      starDictBackend_.close();
      return;
    case DictionaryBackendKind::Japanese:
      japaneseBackend_.close();
      return;
  }
}

void DictionaryEngine::cancel() {
  if (!opened_) return;
  cancelled_ = true;
  cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
  activeEpoch_.store(0, std::memory_order_release);
  activeGeneration_.store(0, std::memory_order_release);
  cancelBackend(backendKind_);
}

void DictionaryEngine::close() {
  activeEpoch_.store(0, std::memory_order_release);
  activeGeneration_.store(0, std::memory_order_release);
  if (japaneseOpened_) closeBackend(DictionaryBackendKind::Japanese);
  if (starDictOpened_) closeBackend(DictionaryBackendKind::StarDict);
  japaneseOpened_ = false;
  starDictOpened_ = false;
  opened_ = false;
  cancelled_ = false;
  currentEpoch_ = 0;
}
