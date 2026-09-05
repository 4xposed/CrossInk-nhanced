#include "StarDictBackend.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr size_t kMaxStarDictWordBytes = 255;

void fnvBytes(uint64_t& hash, const void* data, const size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
}

struct HtmlSpanBridge {
  DictionaryDefinitionSink sink;
  bool accepted = true;

  static void emit(void* context, const StyledSpan& source) {
    auto& self = *static_cast<HtmlSpanBridge*>(context);
    if (!self.accepted || !self.sink.onSpan) {
      self.accepted = false;
      return;
    }
    DictionaryDefinitionSpan span;
    span.text = source.text ? std::string_view(source.text) : std::string_view{};
    span.bold = source.bold;
    span.italic = source.italic;
    span.superscript = source.superscript;
    span.subscript = source.subscript;
    span.ipa = source.ipa;
    span.underline = source.underline;
    span.strikethrough = source.strikethrough;
    span.listItem = source.isListItem;
    span.lineBreak = source.newlineBefore;
    span.indentLevel = source.indentLevel;
    self.accepted = self.sink.onSpan(self.sink.context, span);
  }

  static bool emitControlled(void* context, const StyledSpan& source) {
    emit(context, source);
    return static_cast<HtmlSpanBridge*>(context)->accepted;
  }

  static bool shouldCancel(void* context) { return static_cast<HtmlSpanBridge*>(context)->backend->cancelled(); }

  StarDictBackend* backend = nullptr;
};
}  // namespace

bool StarDictBackend::hashFileSamples(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) return false;
  const size_t fileSize = file.fileSize();
  fnvBytes(signature_, path, std::strlen(path));
  fnvBytes(signature_, &fileSize, sizeof(fileSize));
  const size_t sampleSize = std::min<size_t>(fileSize, 256);
  if (sampleSize != 0) {
    if (!file.seekSet(0) || file.read(plainBuffer_.get(), sampleSize) != static_cast<int>(sampleSize)) {
      file.close();
      return false;
    }
    fnvBytes(signature_, plainBuffer_.get(), sampleSize);
    if (fileSize > sampleSize) {
      if (!file.seekSet(fileSize - sampleSize) ||
          file.read(plainBuffer_.get(), sampleSize) != static_cast<int>(sampleSize)) {
        file.close();
        return false;
      }
      fnvBytes(signature_, plainBuffer_.get(), sampleSize);
    }
  }
  file.close();
  return true;
}

DictionaryStatus StarDictBackend::open(const char* bookCachePath) {
  // Preserve a temporary dictionary override installed by the caller for this
  // new session. Full close() clears it only when the session ends.
  activeSlice_ = DictDefinitionSlice{};
  plainBuffer_.reset();
  htmlRenderer_.reset();
  info_ = DictInfo{};
  capabilities_ = DictionaryCapabilities{};
  signature_ = 0;
  open_ = false;
  bookCachePath_ = bookCachePath;
  cancelled_ = false;
  if (!Dictionary::exists(bookCachePath_)) return DictionaryStatus::Unavailable;

  // Reused across all plain-definition streams. At 512 bytes this is too large
  // for the 4 KiB dictionary worker stack and cannot be static across sessions.
  plainBuffer_ = makeUniqueNoThrow<char[]>(kPlainBufferBytes);
  if (!plainBuffer_) {
    LOG_ERR("DICT", "OOM: %u byte StarDict stream buffer", static_cast<unsigned>(kPlainBufferBytes));
    return DictionaryStatus::OutOfMemory;
  }

  const std::string path = Dictionary::readDictPath(bookCachePath_);
  if (path.empty()) {
    plainBuffer_.reset();
    return DictionaryStatus::Unavailable;
  }
  info_ = Dictionary::readInfo(path.c_str());
  capabilities_.suggestions = true;
  capabilities_.stemVariants = true;
  capabilities_.dictionarySwitch = true;
  signature_ = kFnvOffset;
  fnvBytes(signature_, path.data(), path.size());
  fnvBytes(signature_, &info_.wordcount, sizeof(info_.wordcount));
  fnvBytes(signature_, &info_.idxfilesize, sizeof(info_.idxfilesize));
  fnvBytes(signature_, info_.sametypesequence, std::strlen(info_.sametypesequence));
  const DictPaths paths(path);
  const std::string idxPath = paths.idx();
  const std::string dictPath = paths.dict();
  if (!hashFileSamples(idxPath.c_str()) || !hashFileSamples(dictPath.c_str())) {
    LOG_ERR("DICT", "Failed to fingerprint StarDict files");
    plainBuffer_.reset();
    return DictionaryStatus::ReadError;
  }
  open_ = true;
  return DictionaryStatus::Found;
}

bool StarDictBackend::shouldCancel(void* context) { return static_cast<StarDictBackend*>(context)->cancelled_; }

bool StarDictBackend::cancelled() const { return cancelled_.load(); }

DictionaryStatus StarDictBackend::normalizeQuery(const DictionaryQuery& query, std::string_view& normalized) {
  normalized = {};
  if (!open_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  if (query.byteOffset > query.text.size()) return DictionaryStatus::NotFound;
  const std::string_view token = query.text.substr(query.byteOffset);
  if (token.empty() || token.size() > kMaxStarDictWordBytes) return DictionaryStatus::NotFound;
  if (!plainBuffer_) return DictionaryStatus::OutOfMemory;

  size_t cleanedBytes = 0;
  if (!utf8CleanLookupWordToBuffer(token, plainBuffer_.get(), kPlainBufferBytes, cleanedBytes)) {
    LOG_ERR("DICT", "Could not normalize bounded StarDict query");
    return DictionaryStatus::ReadError;
  }
  if (cleanedBytes == 0 || cleanedBytes > kMaxStarDictWordBytes) return DictionaryStatus::NotFound;
  normalized = std::string_view(plainBuffer_.get(), cleanedBytes);
  return DictionaryStatus::Found;
}

DictionaryStatus StarDictBackend::locate(const DictionaryQuery& query, DictLocation& location, bool& transformed,
                                         bool& alternate, std::string_view& normalized) {
  location = DictLocation{};
  transformed = false;
  alternate = false;
  const DictionaryStatus normalizeStatus = normalizeQuery(query, normalized);
  if (normalizeStatus != DictionaryStatus::Found) return normalizeStatus;

  // The legacy StarDict index/stemming API still owns std::string-based
  // internals. Normalization itself reuses the session's existing 512-byte
  // stream buffer, avoiding another hot-path allocation and remaining
  // recoverable at the backend boundary.
  const std::string lookupWord(normalized);
  bool stemmed = false;
  const DictLookupCallbacks callbacks{this, nullptr, shouldCancel};
  location = Dictionary::locateWithStemVariants(lookupWord, &stemmed, callbacks, bookCachePath_);
  if (cancelled_.load()) return DictionaryStatus::Cancelled;
  if (location.readError) return DictionaryStatus::ReadError;
  if (location.found) {
    transformed = stemmed;
    return DictionaryStatus::Found;
  }

  DictOperationStatus alternateStatus = DictOperationStatus::NotFound;
  const std::string alternateWord = Dictionary::resolveAltForm(lookupWord, bookCachePath_, &alternateStatus);
  if (alternateStatus == DictOperationStatus::ReadError) {
    LOG_ERR("DICT", "Failed reading StarDict alternate forms");
    return DictionaryStatus::ReadError;
  }
  if (alternateStatus == DictOperationStatus::OutOfMemory) return DictionaryStatus::OutOfMemory;
  if (alternateWord.empty()) return DictionaryStatus::NotFound;
  location = Dictionary::locateWithStemVariants(alternateWord, &stemmed, callbacks, bookCachePath_);
  if (cancelled_.load()) return DictionaryStatus::Cancelled;
  if (location.readError) return DictionaryStatus::ReadError;
  if (!location.found) return DictionaryStatus::NotFound;
  transformed = true;
  alternate = true;
  return DictionaryStatus::Found;
}

DictionaryStatus StarDictBackend::probe(const DictionaryQuery& query, DictionaryProbeResult& out) {
  out = DictionaryProbeResult{};
  DictLocation location;
  bool transformed = false;
  bool alternate = false;
  std::string_view normalized;
  const DictionaryStatus status = locate(query, location, transformed, alternate, normalized);
  out.status = status;
  if (status == DictionaryStatus::Found) {
    out.matchedBytes = query.text.size() - query.byteOffset;
    out.transformed = transformed;
  }
  return status;
}

DictionaryStatus StarDictBackend::lookup(const DictionaryQuery& query, DictionaryResult& out) {
  out = DictionaryResult{};
  out.backend = DictionaryBackendKind::StarDict;
  DictLocation location;
  bool transformed = false;
  bool alternate = false;
  std::string_view normalized;
  const DictionaryStatus status = locate(query, location, transformed, alternate, normalized);
  if (status != DictionaryStatus::Found) return out.status = status;

  DictDefinitionSlice slice = Dictionary::resolveDefinitionSlice(location, info_);
  if (cancelled_.load()) return out.status = DictionaryStatus::Cancelled;
  if (!slice.found) return out.status = DictionaryStatus::ReadError;

  DictionaryResult complete;
  complete.status = DictionaryStatus::Found;
  complete.backend = DictionaryBackendKind::StarDict;
  complete.matchedBytes = query.text.size() - query.byteOffset;
  if (!complete.surface.assign(normalized) || !complete.headword.assign(location.headword)) {
    LOG_ERR("DICT", "OOM: StarDict public lookup text");
    return out.status = DictionaryStatus::OutOfMemory;
  }
  if (cancelled_.load()) return out.status = DictionaryStatus::Cancelled;
  complete.transformed = transformed;
  complete.alternate = alternate;
  activeSlice_ = std::move(slice);
  out = std::move(complete);
  return DictionaryStatus::Found;
}

DictionaryStatus StarDictBackend::suggest(const std::string_view word, DictionarySuggestions& out) {
  out = DictionarySuggestions{};
  if (!open_) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  std::string_view normalized;
  const DictionaryStatus normalizeStatus = normalizeQuery({word}, normalized);
  if (normalizeStatus != DictionaryStatus::Found) return normalizeStatus;

  DictOperationStatus suggestionStatus = DictOperationStatus::NotFound;
  const std::vector<std::string> suggestions = Dictionary::findSimilar(
      std::string(normalized), DictionarySuggestions::kCapacity, bookCachePath_, &suggestionStatus);
  if (cancelled_.load()) return DictionaryStatus::Cancelled;
  if (suggestionStatus == DictOperationStatus::ReadError) {
    LOG_ERR("DICT", "Failed reading StarDict suggestions");
    return DictionaryStatus::ReadError;
  }
  if (suggestionStatus == DictOperationStatus::OutOfMemory) return DictionaryStatus::OutOfMemory;
  DictionarySuggestions complete;
  complete.count = static_cast<uint8_t>(std::min<size_t>(suggestions.size(), complete.items.size()));
  for (uint8_t index = 0; index < complete.count; ++index) {
    if (!complete.items[index].assign(suggestions[index])) {
      LOG_ERR("DICT", "OOM: StarDict suggestion text");
      return DictionaryStatus::OutOfMemory;
    }
  }
  if (complete.count == 0) return DictionaryStatus::NotFound;
  out = std::move(complete);
  return DictionaryStatus::Found;
}

DictionaryStatus StarDictBackend::streamPlain(const DictionaryDefinitionSink sink) {
  if (!plainBuffer_) return DictionaryStatus::OutOfMemory;
  const std::string dictPath = DictPaths(activeSlice_.folderPath).dict();
  HalFile file;
  if (!Storage.openFileForRead("DICT", dictPath.c_str(), file)) return DictionaryStatus::ReadError;
  if (!file.seekSet(activeSlice_.offset)) {
    file.close();
    return DictionaryStatus::ReadError;
  }

  uint32_t remaining = activeSlice_.size;
  while (remaining != 0) {
    if (cancelled_.load()) {
      file.close();
      return DictionaryStatus::Cancelled;
    }
    const size_t wanted = std::min<size_t>(remaining, kPlainBufferBytes);
    const int count = file.read(plainBuffer_.get(), wanted);
    if (count <= 0) {
      file.close();
      return DictionaryStatus::ReadError;
    }
    if (cancelled_.load()) {
      file.close();
      return DictionaryStatus::Cancelled;
    }
    const DictionaryDefinitionSpan span{std::string_view(plainBuffer_.get(), static_cast<size_t>(count))};
    if (!sink.onSpan(sink.context, span)) {
      file.close();
      return DictionaryStatus::Cancelled;
    }
    if (cancelled_.load()) {
      file.close();
      return DictionaryStatus::Cancelled;
    }
    remaining -= static_cast<uint32_t>(count);
  }
  file.close();
  return DictionaryStatus::Found;
}

DictionaryStatus StarDictBackend::streamDefinition(const DictionaryDefinitionMode mode,
                                                   const DictionaryDefinitionSink sink) {
  if (!open_ || !activeSlice_.found || !sink.onSpan) return DictionaryStatus::Unavailable;
  if (cancelled_) return DictionaryStatus::Cancelled;
  if (!activeSlice_.isHtml) return streamPlain(sink);
  if (!htmlRenderer_) {
    // StarDict HTML state is cold-path-only and larger than the worker's safe
    // stack budget, so construct it fallibly only for an HTML definition.
    htmlRenderer_ = makeUniqueNoThrow<DictHtmlRenderer>();
    if (!htmlRenderer_) {
      LOG_ERR("DICT", "OOM: StarDict HTML renderer");
      return DictionaryStatus::OutOfMemory;
    }
  }

  const std::string dictPath = DictPaths(activeSlice_.folderPath).dict();
  HtmlSpanBridge bridge{sink, true, this};
  const DictHtmlRenderer::ControlledSpanSink htmlSink{&bridge, HtmlSpanBridge::emitControlled,
                                                      HtmlSpanBridge::shouldCancel};
  const DictHtmlStreamStatus status =
      mode == DictionaryDefinitionMode::Styled
          ? htmlRenderer_->renderFromFileStreamingBuffered(dictPath.c_str(), activeSlice_.offset, activeSlice_.size,
                                                           htmlSink, plainBuffer_.get(), kPlainBufferBytes)
          : htmlRenderer_->renderPlainTextFromFileStreamingBuffered(dictPath.c_str(), activeSlice_.offset,
                                                                    activeSlice_.size, htmlSink, plainBuffer_.get(),
                                                                    kPlainBufferBytes);
  switch (status) {
    case DictHtmlStreamStatus::Success:
      return cancelled_.load() ? DictionaryStatus::Cancelled : DictionaryStatus::Found;
    case DictHtmlStreamStatus::Cancelled:
    case DictHtmlStreamStatus::SinkRejected:
      return DictionaryStatus::Cancelled;
    case DictHtmlStreamStatus::ReadError:
    case DictHtmlStreamStatus::ParseError:
      return DictionaryStatus::ReadError;
    case DictHtmlStreamStatus::OutOfMemory:
      return DictionaryStatus::OutOfMemory;
  }
  return DictionaryStatus::ReadError;
}

DictionaryCapabilities StarDictBackend::capabilities() const { return capabilities_; }

uint64_t StarDictBackend::signature() const { return open_ ? signature_ : 0; }

void StarDictBackend::cancel() { cancelled_ = true; }

void StarDictBackend::close() {
  activeSlice_ = DictDefinitionSlice{};
  plainBuffer_.reset();
  htmlRenderer_.reset();
  info_ = DictInfo{};
  capabilities_ = DictionaryCapabilities{};
  signature_ = 0;
  bookCachePath_ = nullptr;
  open_ = false;
  cancelled_ = false;
  Dictionary::clearLookupDictPathOverride();
}
