#include "DictionaryDefinitionModel.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <utility>

#include "activities/RenderLock.h"

#ifdef CROSSINK_DICT_TESTING
namespace dictionary_definition_model_test {
namespace {
using ReadyPublishHook = void (*)(void*);
ReadyPublishHook readyPublishHook = nullptr;
void* readyPublishContext = nullptr;
using CancelAfterFlagHook = void (*)(void*);
CancelAfterFlagHook cancelAfterFlagHook = nullptr;
void* cancelAfterFlagContext = nullptr;
}  // namespace

void setReadyPublishHook(const ReadyPublishHook hook, void* context) {
  readyPublishHook = hook;
  readyPublishContext = context;
}

void runReadyPublishHook() {
  if (readyPublishHook) readyPublishHook(readyPublishContext);
}

void setCancelAfterFlagHook(const CancelAfterFlagHook hook, void* context) {
  cancelAfterFlagHook = hook;
  cancelAfterFlagContext = context;
}

void runCancelAfterFlagHook() {
  if (cancelAfterFlagHook) cancelAfterFlagHook(cancelAfterFlagContext);
}
}  // namespace dictionary_definition_model_test
#endif

namespace {
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint8_t utf8ExpectedLength(const uint8_t lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

bool decodeUtf8(const char* bytes, const uint8_t length, uint32_t& codepoint) {
  const auto b0 = static_cast<uint8_t>(bytes[0]);
  if (length == 1) {
    codepoint = b0;
    return b0 < 0x80;
  }
  if (length == 2) {
    codepoint = ((b0 & 0x1FU) << 6U) | (static_cast<uint8_t>(bytes[1]) & 0x3FU);
    return b0 >= 0xC2 && b0 <= 0xDF;
  }
  if (length == 3) {
    codepoint = ((b0 & 0x0FU) << 12U) | ((static_cast<uint8_t>(bytes[1]) & 0x3FU) << 6U) |
                (static_cast<uint8_t>(bytes[2]) & 0x3FU);
    return codepoint >= 0x800 && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
  }
  if (length == 4) {
    codepoint = ((b0 & 0x07U) << 18U) | ((static_cast<uint8_t>(bytes[1]) & 0x3FU) << 12U) |
                ((static_cast<uint8_t>(bytes[2]) & 0x3FU) << 6U) | (static_cast<uint8_t>(bytes[3]) & 0x3FU);
    return codepoint >= 0x10000 && codepoint <= 0x10FFFF;
  }
  return false;
}

uint8_t encodeUtf8(const uint32_t codepoint, char out[5]) {
  if (codepoint <= 0x7F) {
    out[0] = static_cast<char>(codepoint);
    out[1] = '\0';
    return 1;
  }
  if (codepoint <= 0x7FF) {
    out[0] = static_cast<char>(0xC0U | (codepoint >> 6U));
    out[1] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    out[2] = '\0';
    return 2;
  }
  if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    out[0] = static_cast<char>(0xE0U | (codepoint >> 12U));
    out[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[2] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    out[3] = '\0';
    return 3;
  }
  if (codepoint <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0U | (codepoint >> 18U));
    out[1] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[3] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    out[4] = '\0';
    return 4;
  }
  return 0;
}

void hashBytes(uint64_t& hash, const void* bytes, const size_t count) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t index = 0; index < count; ++index) {
    hash ^= data[index];
    hash *= kFnvPrime;
  }
}

struct SourceFingerprint {
  uint64_t hash = kFnvOffset;
  uint32_t unitCount = 0;
  uint16_t runFlags = 0;
  uint8_t runIndent = 0;
  bool hasRun = false;
};

bool beginFingerprintUnit(SourceFingerprint& fingerprint, const uint8_t marker) {
  if (fingerprint.unitCount == UINT32_MAX) return false;
  ++fingerprint.unitCount;
  hashBytes(fingerprint.hash, &marker, sizeof(marker));
  return true;
}

bool hashDefinitionSpan(SourceFingerprint& fingerprint, const DictionaryDefinitionSpan& span) {
  uint16_t flags = 0;
  flags |= static_cast<uint16_t>(span.bold) << 0U;
  flags |= static_cast<uint16_t>(span.italic) << 1U;
  flags |= static_cast<uint16_t>(span.superscript) << 2U;
  flags |= static_cast<uint16_t>(span.subscript) << 3U;
  flags |= static_cast<uint16_t>(span.ipa) << 4U;
  flags |= static_cast<uint16_t>(span.underline) << 5U;
  flags |= static_cast<uint16_t>(span.strikethrough) << 6U;
  flags |= static_cast<uint16_t>(span.listItem) << 7U;

  if (span.lineBreak) {
    constexpr uint8_t kLineBreakMarker = 0xB1;
    if (!beginFingerprintUnit(fingerprint, kLineBreakMarker)) return false;
    hashBytes(fingerprint.hash, &span.indentLevel, sizeof(span.indentLevel));
    hashBytes(fingerprint.hash, &span.listItem, sizeof(span.listItem));
    fingerprint.hasRun = false;
  }

  if (span.text.empty()) return true;
  if (!fingerprint.hasRun || fingerprint.runFlags != flags || fingerprint.runIndent != span.indentLevel) {
    constexpr uint8_t kTextRunMarker = 0xC7;
    if (!beginFingerprintUnit(fingerprint, kTextRunMarker)) return false;
    hashBytes(fingerprint.hash, &flags, sizeof(flags));
    hashBytes(fingerprint.hash, &span.indentLevel, sizeof(span.indentLevel));
    fingerprint.runFlags = flags;
    fingerprint.runIndent = span.indentLevel;
    fingerprint.hasRun = true;
  }
  hashBytes(fingerprint.hash, span.text.data(), span.text.size());
  return true;
}

bool checkedAdd(uint32_t& target, const uint32_t amount) {
  if (target > UINT32_MAX - amount) return false;
  target += amount;
  return true;
}

struct PageCounts {
  uint32_t lines = 0;
  uint32_t segments = 0;
  uint32_t textBytes = 0;
};

void hashLine(uint64_t& hash, const DictLayout::LayoutLineView& line) {
  hashBytes(hash, &line.indentLevel, sizeof(line.indentLevel));
  hashBytes(hash, &line.isListItem, sizeof(line.isListItem));
  hashBytes(hash, &line.segmentCount, sizeof(line.segmentCount));
  for (uint16_t index = 0; index < line.segmentCount; ++index) {
    const auto& segment = line.segments[index];
    hashBytes(hash, &segment.length, sizeof(segment.length));
    hashBytes(hash, &segment.style, sizeof(segment.style));
    hashBytes(hash, &segment.isIpa, sizeof(segment.isIpa));
    hashBytes(hash, line.textPool + segment.offset, segment.length);
  }
}

DefinitionBuildState stateForStatus(const DictionaryStatus status) {
  if (status == DictionaryStatus::Cancelled) return DefinitionBuildState::Cancelled;
  if (status == DictionaryStatus::OutOfMemory) return DefinitionBuildState::OutOfMemory;
  return DefinitionBuildState::ReadError;
}
}  // namespace

std::string_view DictionaryDefinitionPage::segmentText(const uint32_t index) const {
  if (!segments || !textPool || index >= segmentCount) return {};
  const auto& segment = segments[index];
  if (segment.textOffset > textPoolBytes || segment.textLength > textPoolBytes - segment.textOffset) return {};
  return std::string_view(textPool + segment.textOffset, segment.textLength);
}

void DictionaryDefinitionPage::clear() {
  ownedTextPool_.reset();
  ownedSegments_.reset();
  ownedLines_.reset();
  lines = nullptr;
  segments = nullptr;
  textPool = nullptr;
  lineCount = 0;
  segmentCount = 0;
  textPoolBytes = 0;
  pageIndex = 0;
}

void DictionaryDefinitionModel::begin(DictionaryEngine& engine, const DictionaryDefinitionHandle handle,
                                      const int targetPage, const int maxWidth, const int linesPerPage) {
  // Ownership guarantees no worker still references the old arrays here.
  page_.clear();
  layoutScratch_.reset();
  advanceTable_ = DefinitionAdvanceTable{};
  narrowFallbackAdvances_.fill(0);
  wideFallbackAdvances_.fill(0);
  engine_ = &engine;
  handle_ = handle;
  requestedPage_ = std::max(0, targetPage);
  maxWidth_ = maxWidth;
  linesPerPage_ = linesPerPage;
  totalPages_ = 0;
  fontId_ = 0;
  indentStep_ = 0;
  bulletWidth_ = 0;
  collectedSourceHash_ = kFnvOffset;
  collectedSourceUnitCount_ = 0;
  styleMask_ = 0;
  codepointTableTruncated_ = false;
  fallbackWidthUsed_ = false;
  plainFallback_ = false;
  cancelRequested_.store(false, std::memory_order_release);
  state_.store(DefinitionBuildState::CollectingCodepoints, std::memory_order_release);
}

void DictionaryDefinitionModel::publishTerminal(const DefinitionBuildState state, const char* message) {
  if (message) LOG_ERR("DICT", "%s", message);
  if (cancelRequested_.load(std::memory_order_acquire)) {
    state_.store(DefinitionBuildState::Cancelled, std::memory_order_release);
    return;
  }
  DefinitionBuildState expected = state_.load(std::memory_order_acquire);
  while (expected == DefinitionBuildState::CollectingCodepoints || expected == DefinitionBuildState::NeedsFontPrewarm ||
         expected == DefinitionBuildState::LayingOut) {
    if (state_.compare_exchange_weak(expected, state, std::memory_order_release, std::memory_order_acquire)) return;
  }
}

void DictionaryDefinitionModel::retainCodepoint(const uint32_t codepoint) {
  if (codepoint == 0 || codepoint == '\r' || codepoint == '\n' || codepoint == '\t') return;
  for (uint16_t index = 0; index < advanceTable_.count; ++index) {
    if (advanceTable_.codepoints[index] == codepoint) return;
  }
  if (advanceTable_.count == DefinitionAdvanceTable::kCodepointCapacity) {
    codepointTableTruncated_ = true;
    return;
  }
  advanceTable_.codepoints[advanceTable_.count++] = codepoint;
}

bool DictionaryDefinitionModel::collectBytes(const std::string_view text, CollectionDecoder& decoder) {
  for (const char value : text) {
    if (cancelRequested_.load(std::memory_order_acquire)) return false;
    const uint8_t byte = static_cast<uint8_t>(value);
    if (decoder.pendingLength == 0) {
      decoder.expectedLength = utf8ExpectedLength(byte);
      if (decoder.expectedLength == 0) return false;
      if (decoder.expectedLength == 1) {
        retainCodepoint(byte);
        continue;
      }
      decoder.pending[0] = value;
      decoder.pendingLength = 1;
      continue;
    }
    if ((byte & 0xC0U) != 0x80U || decoder.pendingLength >= sizeof(decoder.pending)) return false;
    decoder.pending[decoder.pendingLength++] = value;
    if (decoder.pendingLength == decoder.expectedLength) {
      uint32_t codepoint = 0;
      if (!decodeUtf8(decoder.pending, decoder.pendingLength, codepoint)) return false;
      retainCodepoint(codepoint);
      decoder.pendingLength = 0;
      decoder.expectedLength = 0;
    }
  }
  return true;
}

bool DictionaryDefinitionModel::collectSpan(const DictionaryDefinitionSpan& span, CollectionDecoder& decoder) {
  styleMask_ = static_cast<uint8_t>(styleMask_ | (1U << (static_cast<uint8_t>(styleFor(span)) & 0x03U)));
  return collectBytes(span.text, decoder);
}

void DictionaryDefinitionModel::collectCodepointsOnWorker() {
  if (state_.load(std::memory_order_acquire) != DefinitionBuildState::CollectingCodepoints || !engine_) return;
  if (maxWidth_ <= 0 || linesPerPage_ <= 0 || linesPerPage_ > UINT16_MAX) {
    publishTerminal(DefinitionBuildState::ReadError, "Invalid dictionary definition layout dimensions");
    return;
  }

  struct CollectionContext {
    DictionaryDefinitionModel* model = nullptr;
    CollectionDecoder decoder{};
    bool invalidUtf8 = false;
    bool invalidFingerprint = false;
    bool sawValidSpan = false;
    SourceFingerprint fingerprint{};
  } context{this};

  const auto run = [&](const DictionaryDefinitionMode mode) {
    context.decoder = CollectionDecoder{};
    context.invalidUtf8 = false;
    context.invalidFingerprint = false;
    context.sawValidSpan = false;
    context.fingerprint = SourceFingerprint{};
    const DictionaryDefinitionSink sink{&context, [](void* raw, const DictionaryDefinitionSpan& span) {
                                          auto& ctx = *static_cast<CollectionContext*>(raw);
                                          if (ctx.model->cancelRequested_.load(std::memory_order_acquire)) return false;
                                          if (!ctx.model->collectSpan(span, ctx.decoder)) {
                                            if (!ctx.model->cancelRequested_.load(std::memory_order_acquire))
                                              ctx.invalidUtf8 = true;
                                            return false;
                                          }
                                          if (!hashDefinitionSpan(ctx.fingerprint, span)) {
                                            ctx.invalidFingerprint = true;
                                            return false;
                                          }
                                          ctx.sawValidSpan = true;
                                          return !ctx.model->cancelRequested_.load(std::memory_order_acquire);
                                        }};
    const DictionaryStatus status = engine_->streamDefinition(handle_, mode, sink);
    if (context.decoder.pendingLength != 0) context.invalidUtf8 = true;
    return status;
  };

  DictionaryStatus status = run(DictionaryDefinitionMode::Styled);
  if (status == DictionaryStatus::ReadError && !context.invalidUtf8 && !context.invalidFingerprint &&
      context.sawValidSpan && !cancelRequested_.load(std::memory_order_acquire)) {
    // Strict HTML may have emitted a valid prefix. Discard it completely before
    // replaying plain mode so the codepoint table represents one full source.
    advanceTable_ = DefinitionAdvanceTable{};
    codepointTableTruncated_ = false;
    styleMask_ = 0;
    status = run(DictionaryDefinitionMode::PlainFallback);
    plainFallback_ = status == DictionaryStatus::Found && !context.invalidUtf8;
  }
  if (cancelRequested_.load(std::memory_order_acquire) ||
      (status == DictionaryStatus::Cancelled && !context.invalidUtf8 && !context.invalidFingerprint)) {
    publishTerminal(DefinitionBuildState::Cancelled, nullptr);
    return;
  }
  if (context.invalidUtf8 || context.invalidFingerprint || status != DictionaryStatus::Found) {
    publishTerminal(
        context.invalidUtf8 || context.invalidFingerprint ? DefinitionBuildState::ReadError : stateForStatus(status),
        status == DictionaryStatus::OutOfMemory ? "OOM while streaming dictionary definition codepoints"
                                                : "Failed streaming valid dictionary definition codepoints");
    return;
  }

  collectedSourceHash_ = context.fingerprint.hash;
  collectedSourceUnitCount_ = context.fingerprint.unitCount;

  // Roughly 2 KiB and needed across three layout replays. Heap ownership keeps
  // it off the 4-KiB worker stack; fixed capacities prevent growth/fragmentation.
  layoutScratch_ = makeUniqueNoThrow<DictLayout::BoundedWrapScratch>();
  if (!layoutScratch_) {
    publishTerminal(DefinitionBuildState::OutOfMemory, "OOM allocating bounded dictionary layout scratch");
    return;
  }
  DefinitionBuildState expected = DefinitionBuildState::CollectingCodepoints;
  if (!cancelRequested_.load(std::memory_order_acquire) &&
      state_.compare_exchange_strong(expected, DefinitionBuildState::NeedsFontPrewarm, std::memory_order_release,
                                     std::memory_order_acquire)) {
    return;
  }
  publishTerminal(DefinitionBuildState::Cancelled, nullptr);
}

bool DictionaryDefinitionModel::prewarmOnMain(GfxRenderer& renderer, const int fontId, RenderLock& lock) {
  (void)lock;
  if (state_.load(std::memory_order_acquire) != DefinitionBuildState::NeedsFontPrewarm) return false;
  if (!RenderLock::peek()) {
    publishTerminal(DefinitionBuildState::ReadError, "Dictionary font prewarm requires RenderLock");
    return false;
  }
  if (cancelRequested_.load(std::memory_order_acquire)) {
    publishTerminal(DefinitionBuildState::Cancelled, nullptr);
    return false;
  }

  const uint8_t requiredMask = static_cast<uint8_t>(styleMask_ | 0x01U);
  const uint32_t fallbackCodepoints[] = {'?', 0x4E00, '-'};
  renderer.ensureSdCardFontReady(fontId, fallbackCodepoints, 3, /*includeSpace=*/true, /*includeHyphen=*/false,
                                 requiredMask);
  for (uint8_t styleIndex = 0; styleIndex < 4; ++styleIndex) {
    if ((requiredMask & (1U << styleIndex)) == 0) continue;
    const auto style = static_cast<EpdFontFamily::Style>(styleIndex);
    narrowFallbackAdvances_[styleIndex] =
        static_cast<int16_t>(std::clamp(renderer.getTextAdvanceX(fontId, "?", style), 1, INT16_MAX));
    char wide[5]{};
    encodeUtf8(0x4E00, wide);
    wideFallbackAdvances_[styleIndex] =
        static_cast<int16_t>(std::clamp(renderer.getTextAdvanceX(fontId, wide, style),
                                        static_cast<int>(narrowFallbackAdvances_[styleIndex]), INT16_MAX));
  }
  for (uint8_t styleIndex = 1; styleIndex < 4; ++styleIndex) {
    if ((requiredMask & (1U << styleIndex)) != 0) continue;
    narrowFallbackAdvances_[styleIndex] = narrowFallbackAdvances_[0];
    wideFallbackAdvances_[styleIndex] = wideFallbackAdvances_[0];
  }
  indentStep_ = renderer.getTextAdvanceX(fontId, "   ", EpdFontFamily::REGULAR);
  bulletWidth_ = renderer.getTextAdvanceX(fontId, "- ", EpdFontFamily::REGULAR);
  if (indentStep_ < 0 || bulletWidth_ < 0) {
    publishTerminal(DefinitionBuildState::ReadError, "Invalid dictionary list metrics");
    return false;
  }
  // Keep the definition batch last: a full SdCardFont advance cache resets on
  // the next batch with a miss, so fallback-first avoids evicting the active
  // definition immediately before its advances are measured.
  renderer.ensureSdCardFontReady(fontId, advanceTable_.codepoints.data(), advanceTable_.count,
                                 /*includeSpace=*/true, /*includeHyphen=*/false, requiredMask);
  for (uint16_t index = 0; index < advanceTable_.count; ++index) {
    if (cancelRequested_.load(std::memory_order_acquire)) {
      publishTerminal(DefinitionBuildState::Cancelled, nullptr);
      return false;
    }
    char encoded[5]{};
    if (encodeUtf8(advanceTable_.codepoints[index], encoded) == 0) {
      publishTerminal(DefinitionBuildState::ReadError, "Invalid codepoint in dictionary advance table");
      return false;
    }
    const int regularWidth = renderer.getTextAdvanceX(fontId, encoded, EpdFontFamily::REGULAR);
    if (regularWidth < 0 || regularWidth > INT16_MAX) {
      publishTerminal(DefinitionBuildState::ReadError, "Dictionary glyph advance exceeds fixed table range");
      return false;
    }
    advanceTable_.advances[index].fill(static_cast<int16_t>(regularWidth));
    for (uint8_t styleIndex = 1; styleIndex < 4; ++styleIndex) {
      if ((requiredMask & (1U << styleIndex)) == 0) continue;
      const int width = renderer.getTextAdvanceX(fontId, encoded, static_cast<EpdFontFamily::Style>(styleIndex));
      if (width < 0 || width > INT16_MAX) {
        publishTerminal(DefinitionBuildState::ReadError, "Dictionary glyph advance exceeds fixed table range");
        return false;
      }
      advanceTable_.advances[index][styleIndex] = static_cast<int16_t>(width);
    }
  }
  fontId_ = fontId;
  DefinitionBuildState expected = DefinitionBuildState::NeedsFontPrewarm;
  if (!cancelRequested_.load(std::memory_order_acquire) &&
      state_.compare_exchange_strong(expected, DefinitionBuildState::LayingOut, std::memory_order_release,
                                     std::memory_order_acquire)) {
    return true;
  }
  publishTerminal(DefinitionBuildState::Cancelled, nullptr);
  return false;
}

EpdFontFamily::Style DictionaryDefinitionModel::styleFor(const DictionaryDefinitionSpan& span) {
  uint8_t style = span.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  if (span.italic) style = static_cast<uint8_t>(style | EpdFontFamily::ITALIC);
  if (span.underline) style = static_cast<uint8_t>(style | EpdFontFamily::UNDERLINE);
  if (span.strikethrough) style = static_cast<uint8_t>(style | EpdFontFamily::STRIKETHROUGH);
  if (span.superscript) style = static_cast<uint8_t>(style | EpdFontFamily::SUP);
  if (span.subscript) style = static_cast<uint8_t>(style | EpdFontFamily::SUB);
  return static_cast<EpdFontFamily::Style>(style);
}

int DictionaryDefinitionModel::measureFromTable(void* context, const std::string_view text,
                                                const EpdFontFamily::Style style, bool) {
  auto& model = *static_cast<DictionaryDefinitionModel*>(context);
  const uint8_t styleIndex = static_cast<uint8_t>(style) & 0x03U;
  int total = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const uint8_t length = utf8ExpectedLength(static_cast<uint8_t>(text[offset]));
    if (length == 0 || length > text.size() - offset) return -1;
    uint32_t codepoint = 0;
    if (!decodeUtf8(text.data() + offset, length, codepoint)) return -1;
    int width = -1;
    for (uint16_t index = 0; index < model.advanceTable_.count; ++index) {
      if (model.advanceTable_.codepoints[index] == codepoint) {
        width = model.advanceTable_.advances[index][styleIndex];
        break;
      }
    }
    if (width < 0) {
      if (!model.codepointTableTruncated_) return -1;
      model.fallbackWidthUsed_ = true;
      width = utf8IsCombiningMark(codepoint)
                  ? 0
                  : (utf8IsCjkCodepoint(codepoint) ? model.wideFallbackAdvances_[styleIndex]
                                                   : model.narrowFallbackAdvances_[styleIndex]);
    }
    if (width < 0 || total > INT_MAX - width) return -1;
    total += width;
    offset += length;
  }
  return total;
}

void DictionaryDefinitionModel::layoutOnWorker() {
  if (state_.load(std::memory_order_acquire) != DefinitionBuildState::LayingOut || !engine_ || !layoutScratch_) return;

  struct CountContext {
    DictionaryDefinitionModel* model = nullptr;
    uint32_t lineIndex = 0;
    uint32_t currentPage = UINT32_MAX;
    PageCounts current{};
    PageCounts requested{};
    PageCounts last{};
    uint64_t hash = kFnvOffset;
    bool invalid = false;

    static bool accept(void* raw, const DictLayout::LayoutLineView& line) {
      auto& ctx = *static_cast<CountContext*>(raw);
      if (ctx.model->cancelRequested_.load(std::memory_order_acquire)) return false;
      const uint32_t pageIndex = ctx.lineIndex / static_cast<uint32_t>(ctx.model->linesPerPage_);
      if (pageIndex != ctx.currentPage) {
        ctx.currentPage = pageIndex;
        ctx.current = PageCounts{};
      }
      if (!checkedAdd(ctx.current.lines, 1) || !checkedAdd(ctx.current.segments, line.segmentCount)) {
        ctx.invalid = true;
        return false;
      }
      for (uint16_t index = 0; index < line.segmentCount; ++index) {
        const auto& segment = line.segments[index];
        if (segment.offset > DictLayout::BoundedWrapScratch::kTextCapacity ||
            segment.length > DictLayout::BoundedWrapScratch::kTextCapacity - segment.offset ||
            !checkedAdd(ctx.current.textBytes, static_cast<uint32_t>(segment.length) + 1U)) {
          ctx.invalid = true;
          return false;
        }
      }
      if (pageIndex == static_cast<uint32_t>(ctx.model->requestedPage_)) ctx.requested = ctx.current;
      ctx.last = ctx.current;
      hashLine(ctx.hash, line);
      if (ctx.lineIndex == UINT32_MAX) {
        ctx.invalid = true;
        return false;
      }
      ++ctx.lineIndex;
      return !ctx.model->cancelRequested_.load(std::memory_order_acquire);
    }
  } count{this};

  struct ReplayFingerprint {
    SourceFingerprint source{};
    bool acceptedSpan = false;
    bool invalid = false;
  };

  const auto streamLayout = [&](const DictionaryDefinitionMode mode, const DictLayout::ControlledLineSink lineSink,
                                bool& invalid, ReplayFingerprint& fingerprint) {
    fingerprint = ReplayFingerprint{};
    DictLayout::BoundedWrapper wrapper({maxWidth_, indentStep_, bulletWidth_}, {this, measureFromTable}, lineSink,
                                       *layoutScratch_);
    struct SpanContext {
      DictionaryDefinitionModel* model;
      DictLayout::BoundedWrapper* wrapper;
      ReplayFingerprint* fingerprint;
      bool invalid = false;
    } spans{this, &wrapper, &fingerprint};
    const DictionaryDefinitionSink spanSink{
        &spans, [](void* raw, const DictionaryDefinitionSpan& span) {
          auto& ctx = *static_cast<SpanContext*>(raw);
          if (ctx.model->cancelRequested_.load(std::memory_order_acquire)) return false;
          const DictLayout::LengthSpan layoutSpan{span.text,      DictionaryDefinitionModel::styleFor(span),
                                                  span.ipa,       span.listItem,
                                                  span.lineBreak, span.indentLevel};
          if (!ctx.wrapper->onSpan(layoutSpan)) {
            if (ctx.wrapper->status() != DictLayout::BoundedWrapStatus::SinkRejected) ctx.invalid = true;
            return false;
          }
          if (!hashDefinitionSpan(ctx.fingerprint->source, span)) {
            ctx.fingerprint->invalid = true;
            return false;
          }
          ctx.fingerprint->acceptedSpan = true;
          return !ctx.model->cancelRequested_.load(std::memory_order_acquire);
        }};
    DictionaryStatus status = engine_->streamDefinition(handle_, mode, spanSink);
    if (status == DictionaryStatus::Found && !wrapper.finish()) {
      if (wrapper.status() != DictLayout::BoundedWrapStatus::SinkRejected) spans.invalid = true;
      status =
          cancelRequested_.load(std::memory_order_acquire) ? DictionaryStatus::Cancelled : DictionaryStatus::ReadError;
    }
    invalid = spans.invalid || fingerprint.invalid;
    return status;
  };

  bool invalid = false;
  ReplayFingerprint countFingerprint;
  DictionaryDefinitionMode mode =
      plainFallback_ ? DictionaryDefinitionMode::PlainFallback : DictionaryDefinitionMode::Styled;
  DictionaryStatus status = streamLayout(mode, {&count, CountContext::accept}, invalid, countFingerprint);
  if (status == DictionaryStatus::Cancelled && !cancelRequested_.load(std::memory_order_acquire) &&
      (invalid || count.invalid)) {
    status = DictionaryStatus::ReadError;
  }
  if (!plainFallback_ && status == DictionaryStatus::ReadError && countFingerprint.acceptedSpan && !invalid &&
      !count.invalid && !cancelRequested_.load(std::memory_order_acquire)) {
    count = CountContext{this};
    invalid = false;
    mode = DictionaryDefinitionMode::PlainFallback;
    status = streamLayout(mode, {&count, CountContext::accept}, invalid, countFingerprint);
    if (status == DictionaryStatus::Cancelled && !cancelRequested_.load(std::memory_order_acquire) &&
        (invalid || count.invalid)) {
      status = DictionaryStatus::ReadError;
    }
    plainFallback_ = status == DictionaryStatus::Found && !invalid;
  }
  if (cancelRequested_.load(std::memory_order_acquire) ||
      (status == DictionaryStatus::Cancelled && !invalid && !count.invalid)) {
    publishTerminal(DefinitionBuildState::Cancelled, nullptr);
    return;
  }
  if (invalid || count.invalid || status != DictionaryStatus::Found) {
    publishTerminal(stateForStatus(status), status == DictionaryStatus::OutOfMemory
                                                ? "OOM while sizing dictionary definition page"
                                                : "Failed sizing dictionary definition page");
    return;
  }
  if (countFingerprint.source.hash != collectedSourceHash_ ||
      countFingerprint.source.unitCount != collectedSourceUnitCount_) {
    publishTerminal(DefinitionBuildState::ReadError,
                    "Dictionary definition changed between collection and sizing replay");
    return;
  }

  const uint64_t pageCount64 =
      count.lineIndex == 0 ? 1 : (static_cast<uint64_t>(count.lineIndex) + linesPerPage_ - 1U) / linesPerPage_;
  if (pageCount64 == 0 || pageCount64 > INT_MAX) {
    publishTerminal(DefinitionBuildState::ReadError, "Dictionary definition page count overflow");
    return;
  }
  const int pageCount = static_cast<int>(pageCount64);
  const int targetPage = std::min(requestedPage_, pageCount - 1);
  const PageCounts exact = targetPage == requestedPage_ ? count.requested : count.last;
  // BoundedWrapper emits at most 64 segments per line, so the UINT16 line cap
  // also keeps both exact array element counts safely below 32-bit size_t.
  if (exact.lines > UINT16_MAX) {
    publishTerminal(DefinitionBuildState::ReadError, "Dictionary definition snapshot allocation size overflow");
    return;
  }

  DictionaryDefinitionPage complete;
  if (exact.lines != 0) {
    // Exact target-page lifetime allocation. Stack is unsafe because the page
    // outlives the 4-KiB worker call and may contain linesPerPage entries.
    complete.ownedLines_ = makeUniqueNoThrow<DictionaryDefinitionPageLine[]>(exact.lines);
    if (!complete.ownedLines_) {
      publishTerminal(DefinitionBuildState::OutOfMemory, "OOM allocating exact dictionary page lines");
      return;
    }
  }
  if (exact.segments != 0) {
    // Exact target-page segment metadata can exceed 256 bytes and must remain
    // immutable until the activity finishes rendering this publication.
    complete.ownedSegments_ = makeUniqueNoThrow<DictionaryDefinitionPageSegment[]>(exact.segments);
    if (!complete.ownedSegments_) {
      publishTerminal(DefinitionBuildState::OutOfMemory, "OOM allocating exact dictionary page segments");
      return;
    }
  }
  if (exact.textBytes != 0) {
    // One exact pool owns every visible byte plus one NUL per segment. This
    // avoids per-segment strings and keeps renderer-facing C strings safe.
    complete.ownedTextPool_ = makeUniqueNoThrow<char[]>(exact.textBytes);
    if (!complete.ownedTextPool_) {
      publishTerminal(DefinitionBuildState::OutOfMemory, "OOM allocating exact dictionary page text pool");
      return;
    }
  }
  complete.lines = complete.ownedLines_.get();
  complete.segments = complete.ownedSegments_.get();
  complete.textPool = complete.ownedTextPool_.get();
  complete.lineCount = static_cast<uint16_t>(exact.lines);
  complete.segmentCount = exact.segments;
  complete.textPoolBytes = exact.textBytes;
  complete.pageIndex = targetPage;

  struct FillContext {
    DictionaryDefinitionModel* model = nullptr;
    DictionaryDefinitionPage* page = nullptr;
    int targetPage = 0;
    uint32_t lineIndex = 0;
    uint32_t lineCursor = 0;
    uint32_t segmentCursor = 0;
    uint32_t textCursor = 0;
    uint64_t hash = kFnvOffset;
    bool invalid = false;

    static bool accept(void* raw, const DictLayout::LayoutLineView& line) {
      auto& ctx = *static_cast<FillContext*>(raw);
      if (ctx.model->cancelRequested_.load(std::memory_order_acquire)) return false;
      hashLine(ctx.hash, line);
      const uint32_t linePage = ctx.lineIndex / static_cast<uint32_t>(ctx.model->linesPerPage_);
      if (linePage == static_cast<uint32_t>(ctx.targetPage)) {
        if (ctx.lineCursor >= ctx.page->lineCount || ctx.segmentCursor > ctx.page->segmentCount ||
            line.segmentCount > ctx.page->segmentCount - ctx.segmentCursor) {
          ctx.invalid = true;
          return false;
        }
        auto& outputLine = ctx.page->ownedLines_[ctx.lineCursor++];
        outputLine.firstSegment = ctx.segmentCursor;
        outputLine.segmentCount = line.segmentCount;
        outputLine.indentLevel = line.indentLevel;
        outputLine.isListItem = line.isListItem;
        for (uint16_t index = 0; index < line.segmentCount; ++index) {
          const auto& input = line.segments[index];
          if (input.offset > DictLayout::BoundedWrapScratch::kTextCapacity ||
              input.length > DictLayout::BoundedWrapScratch::kTextCapacity - input.offset ||
              ctx.segmentCursor >= ctx.page->segmentCount || ctx.textCursor > ctx.page->textPoolBytes ||
              static_cast<uint32_t>(input.length) + 1U > ctx.page->textPoolBytes - ctx.textCursor) {
            ctx.invalid = true;
            return false;
          }
          auto& output = ctx.page->ownedSegments_[ctx.segmentCursor++];
          output.textOffset = ctx.textCursor;
          output.textLength = input.length;
          output.style = input.style;
          output.isIpa = input.isIpa;
          char* const destination = &ctx.page->ownedTextPool_[ctx.textCursor];
          std::memcpy(destination, line.textPool + input.offset, input.length);
          ctx.textCursor += input.length;
          ctx.page->ownedTextPool_[ctx.textCursor++] = '\0';
        }
      }
      if (ctx.lineIndex == UINT32_MAX) {
        ctx.invalid = true;
        return false;
      }
      ++ctx.lineIndex;
      return !ctx.model->cancelRequested_.load(std::memory_order_acquire);
    }
  } fill{this, &complete, targetPage};

  invalid = false;
  ReplayFingerprint fillFingerprint;
  status = streamLayout(mode, {&fill, FillContext::accept}, invalid, fillFingerprint);
  if (status == DictionaryStatus::Cancelled && !cancelRequested_.load(std::memory_order_acquire) &&
      (invalid || fill.invalid)) {
    status = DictionaryStatus::ReadError;
  }
  if (cancelRequested_.load(std::memory_order_acquire) ||
      (status == DictionaryStatus::Cancelled && !invalid && !fill.invalid)) {
    publishTerminal(DefinitionBuildState::Cancelled, nullptr);
    return;
  }
  if (invalid || fill.invalid || status != DictionaryStatus::Found || fill.lineIndex != count.lineIndex ||
      fill.lineCursor != exact.lines || fill.segmentCursor != exact.segments || fill.textCursor != exact.textBytes ||
      fill.hash != count.hash || fillFingerprint.source.hash != collectedSourceHash_ ||
      fillFingerprint.source.unitCount != collectedSourceUnitCount_ ||
      fillFingerprint.source.hash != countFingerprint.source.hash ||
      fillFingerprint.source.unitCount != countFingerprint.source.unitCount) {
    publishTerminal(stateForStatus(status), status == DictionaryStatus::OutOfMemory
                                                ? "OOM while filling dictionary definition page"
                                                : "Dictionary definition changed between sizing and fill replay");
    return;
  }

  if (codepointTableTruncated_ || fallbackWidthUsed_) {
    LOG_INF("DICT", "Definition advance table capped at %u codepoints; deterministic fallback widths used",
            DefinitionAdvanceTable::kCodepointCapacity);
  }
  if (cancelRequested_.load(std::memory_order_acquire)) {
    publishTerminal(DefinitionBuildState::Cancelled, nullptr);
    return;
  }
  totalPages_ = pageCount;
  page_ = std::move(complete);
  DefinitionBuildState expected = DefinitionBuildState::LayingOut;
#ifdef CROSSINK_DICT_TESTING
  dictionary_definition_model_test::runReadyPublishHook();
#endif
  if (state_.compare_exchange_strong(expected, DefinitionBuildState::Ready, std::memory_order_release,
                                     std::memory_order_acquire)) {
    return;
  }
  page_.clear();
  totalPages_ = 0;
  publishTerminal(DefinitionBuildState::Cancelled, nullptr);
}

void DictionaryDefinitionModel::cancel() {
  DefinitionBuildState current = state_.load(std::memory_order_acquire);
  while (current == DefinitionBuildState::CollectingCodepoints || current == DefinitionBuildState::NeedsFontPrewarm ||
         current == DefinitionBuildState::LayingOut) {
    if (state_.compare_exchange_weak(current, DefinitionBuildState::Cancelled, std::memory_order_release,
                                     std::memory_order_acquire)) {
      break;
    }
  }
  cancelRequested_.store(true, std::memory_order_release);
#ifdef CROSSINK_DICT_TESTING
  dictionary_definition_model_test::runCancelAfterFlagHook();
#endif
  if (engine_) engine_->cancel();
}

void DictionaryDefinitionModel::clear() {
  // The owning activity cancels and joins the worker before reaching here.
  page_.clear();
  layoutScratch_.reset();
  advanceTable_ = DefinitionAdvanceTable{};
  narrowFallbackAdvances_.fill(0);
  wideFallbackAdvances_.fill(0);
  engine_ = nullptr;
  handle_ = DictionaryDefinitionHandle{};
  requestedPage_ = 0;
  maxWidth_ = 0;
  linesPerPage_ = 1;
  totalPages_ = 0;
  fontId_ = 0;
  indentStep_ = 0;
  bulletWidth_ = 0;
  collectedSourceHash_ = 0;
  collectedSourceUnitCount_ = 0;
  styleMask_ = 0;
  codepointTableTruncated_ = false;
  fallbackWidthUsed_ = false;
  plainFallback_ = false;
  cancelRequested_.store(false, std::memory_order_release);
  state_.store(DefinitionBuildState::Idle, std::memory_order_release);
}
