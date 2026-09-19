#include "JapaneseDictionaryBackend.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "Epub/RubyGlossary.h"

namespace {
constexpr size_t MAX_BOOK_CACHE_PATH_BYTES = 512;

bool nextCharacter(std::string_view text, size_t& offset, uint32_t& cp) {
  if (offset >= text.size()) return false;
  const auto lead = static_cast<uint8_t>(text[offset]);
  const size_t bytes = lead < 0x80                    ? 1
                       : lead >= 0xC2 && lead <= 0xDF ? 2
                       : lead >= 0xE0 && lead <= 0xEF ? 3
                       : lead >= 0xF0 && lead <= 0xF4 ? 4
                                                      : 0;
  if (!bytes || bytes > text.size() - offset) return false;
  cp = lead & (bytes == 1 ? 0x7F : bytes == 2 ? 0x1F : bytes == 3 ? 0x0F : 0x07);
  for (size_t i = 1; i < bytes; ++i) {
    const auto next = static_cast<uint8_t>(text[offset + i]);
    if ((next & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (next & 0x3F);
  }
  if ((bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return false;
  offset += bytes;
  return true;
}

}  // namespace

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
    out.usuallyKana = probe.usuallyKana;
    out.markerCheckFailed = probe.markerCheckFailed;
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
    grammarEntry_.reset();
    grammarLabel_.reset();
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

  grammarEntry_.reset();
  grammarLabel_.reset();
  activeEntry_ = std::move(lookupResult.entry);
  applyGrammar(query, complete);
  syntheticNameDefinition_.reset();
  out = std::move(complete);
  return DictionaryStatus::Found;
}

void JapaneseDictionaryBackend::applyGrammar(const DictionaryQuery& query, const DictionaryResult& result) {
  if (!capabilities_.grammar || cancelled_) return;
  char displayed[DictIndexRecord::HEADWORD_SIZE]{};
  const auto headword = result.headword.view();
  if (query.displayPrefix.size() >= sizeof(displayed) ||
      headword.size() >= sizeof(displayed) - query.displayPrefix.size())
    return;
  const size_t displayedBytes = query.displayPrefix.size() + headword.size();
  if (!query.displayPrefix.empty()) std::memcpy(displayed, query.displayPrefix.data(), query.displayPrefix.size());
  std::memcpy(displayed + query.displayPrefix.size(), headword.data(), headword.size());
  const std::string_view displayedHeadword(displayed, displayedBytes);
  size_t offset = 0;
  uint8_t characters = 0;
  bool hiragana = !result.surface.empty();
  const auto surface = result.surface.view();
  while (hiragana && offset < surface.size()) {
    uint32_t cp = 0;
    hiragana = nextCharacter(surface, offset, cp) && cp >= 0x3040 && cp <= 0x309F && ++characters <= 3;
  }
  if (hiragana) {
    DictEntry preferred;
    const auto status = index_.lookupExact(displayedHeadword, preferred, DictIndex::DICT_GRAMMAR);
    if (status == JapaneseDictStatus::Found) {
      activeEntry_.definition = std::move(preferred.definition);
      activeEntry_.definitionLength = preferred.definitionLength;
    } else if (status != JapaneseDictStatus::NotFound) {
      LOG_ERR("DICT", "Optional short-word grammar lookup failed");
    }
  }
  if (cancelled_ || query.grammarContext.empty() || ESP.getMaxAllocHeap() < 16 * 1024) return;
  // At most 13 characters from the activity's stable paragraph window.
  const auto context = query.grammarContext;
  if (context.size() > 52 || query.grammarCursorByteOffset >= context.size()) return;
  uint8_t ends[14]{};
  uint8_t count = 0;
  int cursor = -1;
  offset = 0;
  while (offset < context.size() && count < 13) {
    if (offset == query.grammarCursorByteOffset) cursor = count;
    uint32_t cp = 0;
    if (!nextCharacter(context, offset, cp)) return;
    ends[++count] = static_cast<uint8_t>(offset);
  }
  if (offset != context.size() || cursor < 0) return;
  std::string_view best;
  uint8_t bestLength = 0;
  for (int start = std::max(0, cursor - 3); start <= cursor; ++start) {
    for (int length = std::min(10, static_cast<int>(count) - start); length >= 2; --length) {
      if (cancelled_) return;
      const auto word = context.substr(ends[start], ends[start + length] - ends[start]);
      DictProbe probe;
      const auto status = index_.probeExact(word, probe, DictIndex::DICT_GRAMMAR);
      if (status == JapaneseDictStatus::Found) {
        if (word != displayedHeadword && length > bestLength) {
          best = word;
          bestLength = length;
        }
        break;
      }
      if (status != JapaneseDictStatus::NotFound) {
        LOG_ERR("DICT", "Optional grammar context probe failed");
        return;
      }
    }
  }
  if (best.empty() || cancelled_) return;
  if (index_.lookupExact(best, grammarEntry_, DictIndex::DICT_GRAMMAR) != JapaneseDictStatus::Found ||
      !grammarLabel_.assign(query.grammarLabel)) {
    LOG_ERR("DICT", "Optional grammar definition could not be retained");
    grammarEntry_.reset();
  }
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
  if (!sink.onSpan(sink.context, span) || cancelled_) return DictionaryStatus::Cancelled;
  if (grammarEntry_.definition) {
    const std::string_view parts[] = {"\n\n— ", grammarLabel_.view(),          ": ", grammarEntry_.headwordView(),
                                      " —\n",   grammarEntry_.definitionView()};
    for (const auto part : parts) {
      if (cancelled_ || !sink.onSpan(sink.context, DictionaryDefinitionSpan{part})) return DictionaryStatus::Cancelled;
    }
  }
  return DictionaryStatus::Found;
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
  grammarEntry_.reset();
  grammarLabel_.reset();
  activeEntry_.reset();
  syntheticNameDefinition_.reset();
  index_.close();
  capabilities_ = DictionaryCapabilities{};
  bookCachePath_.reset();
  bookCachePathLength_ = 0;
  open_ = false;
  cancelled_ = false;
}
