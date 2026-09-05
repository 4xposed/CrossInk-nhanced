#include "JapaneseDictionaryBackend.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <utility>

#include "Epub/RubyGlossary.h"

namespace {
constexpr size_t MAX_BOOK_CACHE_PATH_BYTES = 512;
}

static_assert(DictionaryProbeResult::kReadingRecord == DictIndexRecord::POS_READING);

DictionaryStatus JapaneseDictionaryBackend::mapStatus(const JapaneseDictStatus status) {
  switch (status) {
    case JapaneseDictStatus::Found:
      return DictionaryStatus::Found;
    case JapaneseDictStatus::NotFound:
      return DictionaryStatus::NotFound;
    case JapaneseDictStatus::Unavailable:
      return DictionaryStatus::Unavailable;
    case JapaneseDictStatus::ReadError:
      return DictionaryStatus::ReadError;
    case JapaneseDictStatus::OutOfMemory:
      return DictionaryStatus::OutOfMemory;
  }
  return DictionaryStatus::ReadError;
}

DictionaryStatus JapaneseDictionaryBackend::open(const char* bookCachePath) {
  close();
  cancelled_ = false;
  const JapaneseDictStatus status = index_.open();
  if (status != JapaneseDictStatus::Found) return mapStatus(status);

  const uint8_t sources = index_.availableSources();
  capabilities_.deinflection = true;
  capabilities_.names = (sources & DictIndex::DICT_NAMES) != 0;
  capabilities_.grammar = (sources & DictIndex::DICT_GRAMMAR) != 0;
  if (bookCachePath && bookCachePath[0] != '\0') {
    size_t length = 0;
    while (length <= MAX_BOOK_CACHE_PATH_BYTES && bookCachePath[length] != '\0') ++length;
    if (length > MAX_BOOK_CACHE_PATH_BYTES) {
      LOG_ERR("DICT", "Book cache path exceeds ruby lookup limit");
    } else {
      // The request path belongs to the caller. This exact session-scoped copy
      // survives later worker lookups and is released by close()/reopen.
      auto ownedPath = makeUniqueNoThrow<char[]>(length + 1);
      if (!ownedPath) {
        LOG_ERR("DICT", "OOM allocating %u-byte ruby book path", static_cast<unsigned>(length + 1));
      } else {
        std::memcpy(ownedPath.get(), bookCachePath, length + 1);
        bookCachePath_ = std::move(ownedPath);
        bookCachePathLength_ = length;
        capabilities_.ruby = true;
      }
    }
  }
  open_ = true;
  return DictionaryStatus::Found;
}

DictionaryStatus JapaneseDictionaryBackend::probe(const DictionaryQuery& query, DictionaryProbeResult& out) {
  out = DictionaryProbeResult{};
  if (!open_) return out.status = DictionaryStatus::Unavailable;
  if (cancelled_) return out.status = DictionaryStatus::Cancelled;

  WordLookupProbe probe;
  const DictionaryStatus status = mapStatus(lookup_.probe(query.text, query.byteOffset, probe));
  if (cancelled_.load()) return out.status = DictionaryStatus::Cancelled;
  out.status = status;
  if (status == DictionaryStatus::Found) {
    out.matchedBytes = probe.matchLength;
    out.transformed = probe.deinflected;
    out.sourceMask = probe.sourceDict;
    out.priority = probe.priority;
    out.posFlags = probe.posFlags;
  }
  return status;
}

DictionaryStatus JapaneseDictionaryBackend::lookup(const DictionaryQuery& query, DictionaryResult& out) {
  out = DictionaryResult{};
  out.backend = DictionaryBackendKind::Japanese;
  if (!open_) return out.status = DictionaryStatus::Unavailable;
  if (cancelled_) return out.status = DictionaryStatus::Cancelled;

  WordLookupResult lookupResult;
  const DictionaryStatus status = mapStatus(lookup_.lookup(query.text, query.byteOffset, lookupResult));
  if (cancelled_.load()) return out.status = DictionaryStatus::Cancelled;
  if (status != DictionaryStatus::Found) return out.status = status;
  if (lookupResult.matchLength > DictIndexRecord::HEADWORD_SIZE - 1 || query.byteOffset > query.text.size() ||
      lookupResult.matchLength > query.text.size() - query.byteOffset) {
    return out.status = DictionaryStatus::ReadError;
  }

  DictionaryResult complete;
  complete.status = DictionaryStatus::Found;
  complete.backend = DictionaryBackendKind::Japanese;
  if (query.synthesizePartialKatakanaName && lookupResult.matchLength < query.text.size() && query.byteOffset == 0 &&
      query.text.size() <= DictIndexRecord::HEADWORD_SIZE - 1 && !query.syntheticNameDefinition.empty()) {
    complete.matchedBytes = query.text.size();
    if (!complete.surface.assign(query.text) || !complete.headword.assign(query.text)) {
      LOG_ERR("DICT", "OOM: synthetic Japanese name text");
      return out.status = DictionaryStatus::OutOfMemory;
    }
    DictionaryOwnedText syntheticDefinition;
    if (!syntheticDefinition.assign(query.syntheticNameDefinition)) {
      LOG_ERR("DICT", "OOM: synthetic Japanese name definition");
      return out.status = DictionaryStatus::OutOfMemory;
    }
    complete.syntheticName = true;
    complete.sourceMask = DictIndex::DICT_NAMES;
    activeEntry_.reset();
    syntheticNameDefinition_ = std::move(syntheticDefinition);
    out = std::move(complete);
    return DictionaryStatus::Found;
  }
  complete.matchedBytes = lookupResult.matchLength;
  // Both values are bounded to 31 bytes by the on-disk record contract. Each
  // exact-size public text allocation is fallible and occurs before publication.
  if (!complete.surface.assign(query.text.substr(query.byteOffset, lookupResult.matchLength)) ||
      !complete.headword.assign(lookupResult.entry.headwordView())) {
    LOG_ERR("DICT", "OOM: Japanese public lookup text");
    return out.status = DictionaryStatus::OutOfMemory;
  }
  if (cancelled_.load()) return out.status = DictionaryStatus::Cancelled;
  complete.transformed = lookupResult.deinflected;
  complete.sourceMask = lookupResult.entry.sourceDict;

  activeEntry_ = std::move(lookupResult.entry);
  syntheticNameDefinition_.reset();
  out = std::move(complete);
  return DictionaryStatus::Found;
}

DictionaryStatus JapaneseDictionaryBackend::suggest(std::string_view, DictionarySuggestions& out) {
  out = DictionarySuggestions{};
  if (!open_) return DictionaryStatus::Unavailable;
  return cancelled_ ? DictionaryStatus::Cancelled : DictionaryStatus::Unavailable;
}

DictionaryStatus JapaneseDictionaryBackend::streamDefinition(DictionaryDefinitionMode, DictionaryDefinitionSink sink) {
  if (!open_ || (!activeEntry_.definition && syntheticNameDefinition_.empty())) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  if (!sink.onSpan) return DictionaryStatus::Unavailable;
  const DictionaryDefinitionSpan span{syntheticNameDefinition_.empty() ? activeEntry_.definitionView()
                                                                       : syntheticNameDefinition_.view()};
  const bool accepted = sink.onSpan(sink.context, span);
  return !accepted || cancelled_.load() ? DictionaryStatus::Cancelled : DictionaryStatus::Found;
}

bool JapaneseDictionaryBackend::bookReading(const std::string_view surface, DictionaryOwnedText& out) {
  out.reset();
  if (!open_ || cancelled_ || !bookCachePath_ || surface.empty()) return false;
  RubyGlossary::OwnedReadings readings;
  const RubyGlossary::LookupStatus status =
      RubyGlossary::lookupOwned(std::string_view(bookCachePath_.get(), bookCachePathLength_), surface, readings);
  if (cancelled_.load() || status != RubyGlossary::LookupStatus::Found) return false;
  const size_t length = readings.length();
  DictionaryOwnedText complete;
  if (!complete.adopt(readings.releaseStorage(), length, length + 1)) {
    LOG_ERR("DICT", "Failed to adopt ruby reading output");
    return false;
  }
  out = std::move(complete);
  return true;
}

DictionaryCapabilities JapaneseDictionaryBackend::capabilities() const { return capabilities_; }

uint64_t JapaneseDictionaryBackend::signature() const { return open_ ? index_.signature() : 0; }

void JapaneseDictionaryBackend::cancel() {
  // The definition stream borrows activeEntry_. Cancellation may race that
  // callback, so only the owner-safe close/replacement path releases storage.
  cancelled_.store(true);
}

void JapaneseDictionaryBackend::close() {
  activeEntry_.reset();
  syntheticNameDefinition_.reset();
  index_.close();
  capabilities_ = DictionaryCapabilities{};
  bookCachePath_.reset();
  bookCachePathLength_ = 0;
  open_ = false;
  cancelled_ = false;
}
