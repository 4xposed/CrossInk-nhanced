#include "EpubReaderWordLookupActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "DictionarySuggestionsActivity.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "activities/settings/DictionarySelectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryActivityUtils.h"
#include "util/DictionaryLookupWorker.h"
#include "util/DictionaryRegistry.h"
#include "util/LookupHistory.h"

namespace {
constexpr char kEllipsis[] = "\xE2\x80\xA6";
constexpr char kBullet[] = "- ";

bool decodeUtf8(const std::string_view text, const size_t offset, uint32_t& codepoint, size_t& length) {
  if (offset >= text.size()) return false;
  const uint8_t lead = static_cast<uint8_t>(text[offset]);
  uint32_t minimum = 0;
  if (lead < 0x80) {
    codepoint = lead;
    length = 1;
    return true;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    codepoint = lead & 0x1FU;
    length = 2;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    codepoint = lead & 0x0FU;
    length = 3;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    codepoint = lead & 0x07U;
    length = 4;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (length > text.size() - offset) return false;
  for (size_t index = 1; index < length; ++index) {
    const uint8_t next = static_cast<uint8_t>(text[offset + index]);
    if ((next & 0xC0U) != 0x80U) return false;
    codepoint = (codepoint << 6U) | (next & 0x3FU);
  }
  return codepoint >= minimum && codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
}

bool definitionTokenDelimiter(const uint32_t codepoint) {
  if (codepoint <= 0x20 || codepoint == 0x7F || codepoint == 0x3000) return true;
  if (codepoint < 0x80) {
    switch (codepoint) {
      case '!':
      case '"':
      case '#':
      case '$':
      case '%':
      case '&':
      case '(':
      case ')':
      case '*':
      case '+':
      case ',':
      case '.':
      case '/':
      case ':':
      case ';':
      case '<':
      case '=':
      case '>':
      case '?':
      case '@':
      case '[':
      case '\\':
      case ']':
      case '^':
      case '_':
      case '`':
      case '{':
      case '|':
      case '}':
      case '~':
        return true;
      default:
        return false;
    }
  }
  return (codepoint >= 0x2000 && codepoint <= 0x206F) || (codepoint >= 0x3001 && codepoint <= 0x303F) ||
         codepoint == 0xFF0C || codepoint == 0xFF0E || codepoint == 0xFF1A || codepoint == 0xFF1B ||
         codepoint == 0xFF1F;
}

uint8_t utf8Length(const uint32_t codepoint) {
  if (codepoint <= 0x7F) return 1;
  if (codepoint <= 0x7FF) return 2;
  if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) return 3;
  return codepoint <= 0x10FFFF ? 4 : 0;
}

bool appendCodepoint(const uint32_t codepoint, char* output, const size_t capacity, size_t& used) {
  const uint8_t count = utf8Length(codepoint);
  if (count == 0 || used > capacity || count > capacity - used) return false;
  if (count == 1) {
    output[used++] = static_cast<char>(codepoint);
  } else if (count == 2) {
    output[used++] = static_cast<char>(0xC0U | (codepoint >> 6U));
    output[used++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
  } else if (count == 3) {
    output[used++] = static_cast<char>(0xE0U | (codepoint >> 12U));
    output[used++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    output[used++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
  } else {
    output[used++] = static_cast<char>(0xF0U | (codepoint >> 18U));
    output[used++] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
    output[used++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    output[used++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
  }
  return true;
}

bool isJapaneseDigit(const uint32_t codepoint) {
  return (codepoint >= '0' && codepoint <= '9') || (codepoint >= 0xFF10 && codepoint <= 0xFF19);
}

bool isJapaneseKatakana(const uint32_t codepoint) {
  return (codepoint >= 0x30A0 && codepoint <= 0x30FF) || codepoint == 0x30FC;
}

MappedInputManager::Button lookupNavigationButton(const DictionaryLookupNavigationButton button) {
  switch (button) {
    case DictionaryLookupNavigationButton::Left:
      return MappedInputManager::Button::Left;
    case DictionaryLookupNavigationButton::Right:
      return MappedInputManager::Button::Right;
    case DictionaryLookupNavigationButton::Up:
      return MappedInputManager::Button::Up;
    case DictionaryLookupNavigationButton::Down:
      return MappedInputManager::Button::Down;
  }
  return MappedInputManager::Button::Down;
}

LookupHistory::Status historyStatus(const DictionaryResult& result, const bool suggestion) {
  switch (dictionaryLookupHistoryKind(result, suggestion)) {
    case DictionaryLookupHistoryKind::Direct:
      return LookupHistory::Status::Direct;
    case DictionaryLookupHistoryKind::Stem:
      return LookupHistory::Status::Stem;
    case DictionaryLookupHistoryKind::AltForm:
      return LookupHistory::Status::AltForm;
    case DictionaryLookupHistoryKind::Suggestion:
      return LookupHistory::Status::Suggestion;
  }
  return LookupHistory::Status::Direct;
}
}  // namespace

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::unique_ptr<Page> page, EpubLookupPageRequest request)
    : Activity("EpubReaderWordLookup", renderer, mappedInput), page_(std::move(page)), pageMode_(true) {
  initializeRequest(std::move(request));
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           std::string directWord, EpubLookupPageRequest request)
    : Activity("EpubReaderWordLookup", renderer, mappedInput), directWord_(std::move(directWord)), pageMode_(false) {
  initializeRequest(std::move(request));
}

EpubReaderWordLookupActivity::~EpubReaderWordLookupActivity() {
  if (!shutdownComplete_) LOG_ERR("WLA", "Lookup activity destroyed before its resources were drained");
}

void EpubReaderWordLookupActivity::initializeRequest(EpubLookupPageRequest&& request) {
  bookLanguage_ = std::move(request.bookLanguage);
  bookCachePath_ = std::move(request.bookCachePath);
  spineIndex_ = request.spineIndex;
  pageIndex_ = request.pageIndex;
  marginLeft_ = request.marginLeft;
  marginTop_ = request.marginTop;
  reservedBottomHeight_ = std::max(0, request.reservedBottomHeight);
  initialTouchX_ = request.initialTouchX;
  initialTouchY_ = request.initialTouchY;
  autoLookupInitialWord_ = request.autoLookupInitialWord;
  framebufferContainsPage_ = request.framebufferContainsPage;
  initialRecordLookupHistory_ = request.recordLookupHistory;
  dictionaryFontPointSize_ = request.dictionaryFontPointSize;
  readerContext_ = request.readerContext;
  readerBackgroundRender_ = request.renderReaderBackground;
  readerPageReload_ = request.reloadReaderPage;
  if (request.dictionaryFontFamilyName) {
    size_t length = 0;
    while (length < dictionaryFontFamilyName_.size() && request.dictionaryFontFamilyName[length] != '\0') ++length;
    if (length == dictionaryFontFamilyName_.size()) {
      LOG_ERR("WLA", "Dictionary font family exceeds %u bytes; truncating",
              static_cast<unsigned>(dictionaryFontFamilyName_.size() - 1));
      length = dictionaryFontFamilyName_.size() - 1;
    }
    std::memcpy(dictionaryFontFamilyName_.data(), request.dictionaryFontFamilyName, length);
    dictionaryFontFamilyName_[length] = '\0';
  }
}

DictionaryStatus EpubReaderWordLookupActivity::openRoutedEngine() {
  const DictionaryOpenRequest request{bookLanguage_, bookCachePath_.empty() ? nullptr : bookCachePath_.c_str()};
  if (!dictionaryOverridePath_.empty()) return engine_.openStarDictOverride(request, dictionaryOverridePath_.c_str());

  std::string starDictPath;
  // The helper destroys its private catalog before the engine opens the one
  // long-lived dictionary session; the shared Settings catalog stays intact.
  const bool available = resolveTransientDictionaryLookupRoute(
      bookLanguage_, bookCachePath_.empty() ? nullptr : bookCachePath_.c_str(), starDictPath);
  if (!available) return DictionaryStatus::Unavailable;
  if (!starDictPath.empty()) return engine_.openStarDictOverride(request, starDictPath.c_str());
  return engine_.open(request);
}

DictionaryStatus EpubReaderWordLookupActivity::openEngine() {
  const DictionaryStatus status = openRoutedEngine();
  engineOpen_ = status == DictionaryStatus::Found;
  if (!engineOpen_) {
    LOG_ERR("WLA", "Dictionary engine open failed with status %u", static_cast<unsigned>(status));
    capabilities_ = {};
    return status;
  }
  capabilities_ = engine_.capabilities();
  cacheBackend_ = engine_.backendKind();
  cacheDictionarySignature_ = engine_.signature();
  cacheIdentityValid_ = true;
  return DictionaryStatus::Found;
}

DictionaryStatus EpubReaderWordLookupActivity::reopenCancelledEngine() {
  engine_.close();
  const DictionaryStatus status = openRoutedEngine();
  engineOpen_ = status == DictionaryStatus::Found;
  capabilities_ = engineOpen_ ? engine_.capabilities() : DictionaryCapabilities{};
  if (!engineOpen_) LOG_ERR("WLA", "Dictionary engine reopen failed with status %u", static_cast<unsigned>(status));
  if (engineOpen_) {
    cacheBackend_ = engine_.backendKind();
    cacheDictionarySignature_ = engine_.signature();
    cacheIdentityValid_ = true;
  }
  return status;
}

DictionaryStatus EpubReaderWordLookupActivity::initializePageMode(const bool deferInitialSelection) {
  if (!page_ && readerPageReload_) {
    RenderLock lock(*this);
    page_ = readerPageReload_(readerContext_);
  }
  if (!page_) {
    LOG_ERR("WLA", "Cannot open page lookup without a reader page");
    return DictionaryStatus::ReadError;
  }

  DictionaryStatus sourceStatus = DictionaryStatus::Unavailable;
  {
    RenderLock lock(*this);
    sourceStatus = pageSource_.build(*page_, renderer, SETTINGS.getReaderFontId(), marginLeft_, marginTop_);
  }
  if (sourceStatus != DictionaryStatus::Found) {
    LOG_ERR("WLA", "Could not build page text source: %u", static_cast<unsigned>(sourceStatus));
    return sourceStatus;
  }

  scanCachePath_[0] = '\0';
  if (!bookCachePath_.empty()) {
    const int written =
        std::snprintf(scanCachePath_.data(), scanCachePath_.size(), "%s/wlscan.bin", bookCachePath_.c_str());
    if (written < 0 || static_cast<size_t>(written) >= scanCachePath_.size()) {
      scanCachePath_[0] = '\0';
      LOG_ERR("WLA", "Book cache path is too long for wlscan.bin");
    }
  }

  const PageTextSourceView source = pageSource_.view();
  const PageWordScanCacheIdentity identity{
      cacheBackend_, spineIndex_, pageIndex_, source.contentHash, cacheDictionarySignature_, source.glyphCount};
  cacheLoaded_ = scanCachePath_[0] != '\0' && scanCache_.load(scanCachePath_.data(), identity);
  if (cacheLoaded_) {
    flow_.beginPage(openedAtMs_, scanCache_.candidateCount(), true, scanCache_.cursor(), deferInitialSelection);
    return DictionaryStatus::Found;
  }

  const DictionaryProbeFn probe{this, [](void* context, const DictionaryQuery& query, DictionaryProbeResult& out) {
                                  return static_cast<EpubReaderWordLookupActivity*>(context)->engine_.probe(query, out);
                                }};
  const PageWordScannerMemoryRecoveryFn memoryRecovery{
      this, [](void* context) {
        auto& self = *static_cast<EpubReaderWordLookupActivity*>(context);
        RenderLock lock(self);
        const int readerFontId = SETTINGS.getReaderFontId();
        bool released = self.renderer.releaseSdCardFontForLowMemory(readerFontId);
        if (self.dictionaryFontActive_ && self.definitionFontId_ != readerFontId) {
          released = self.renderer.releaseSdCardFontForLowMemory(self.definitionFontId_) || released;
        }
        if (auto* cache = self.renderer.getFontCacheManager()) cache->clearCache();
        LOG_INF("WLA", "Page scanner retried full capacity after font-cache release attempt (%s)",
                released ? "released" : "no eligible SD font");
      }};
  const DictionaryStatus scanStatus = scanner_.begin(source, engine_.backendKind(), probe, memoryRecovery);
  if (scanStatus != DictionaryStatus::Found) {
    LOG_ERR("WLA", "Could not initialize page scanner: %u", static_cast<unsigned>(scanStatus));
    return scanStatus;
  }
  if (scanner_.truncated()) {
    LOG_ERR("WLA", "Page scanner remains capacity-limited after one font-cache recovery retry");
  }
  flow_.beginPage(openedAtMs_, 0, scanner_.completedSuccessfully() && !scanner_.truncated(), 0, deferInitialSelection);
  return DictionaryStatus::Found;
}

void EpubReaderWordLookupActivity::initializeDirectMode() { flow_.beginDirect(openedAtMs_); }

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("WLA", "Lookup enter heap free=%u maxAlloc=%u activity=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(sizeof(*this)));
  openedAtMs_ = millis();
  mappedInput.setReaderTouchscreenOverride(true);
  ignoreInitialBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  exiting_ = false;
  shutdownComplete_ = false;
  renderDisabled_ = false;
  initialRender_ = true;
  fastRefreshCount_ = 0;
  readyTimeLogged_ = false;
  notFoundHistoryRecorded_ = false;
  suggestionsShownForGeneration_ = false;
  openDeadlineLogged_ = false;
  syntheticNameDefinition_ = tr(STR_LOOKUP_NAME);
  definitionSelectionMode_ = false;
  dictionarySwitchHeld_ = false;
  initialTouchMiss_ = false;
  recordLookupHistory_ = initialRecordLookupHistory_;
  notFoundShouldRecordHistory_ = true;
  resetDefinitionBackChain();
  pendingInitialTouchSelection_ = pageMode_ && initialTouchX_ >= 0 && initialTouchY_ >= 0;
  dismissOnInitialTouchMiss_ = pendingInitialTouchSelection_ && autoLookupInitialWord_;
  nearestOnInitialTouchMiss_ = pendingInitialTouchSelection_ && !autoLookupInitialWord_;

  const DictionaryStatus openStatus = openEngine();
  DictionaryStatus initializationStatus = openStatus;
  if (openStatus == DictionaryStatus::Found) {
    if (pageMode_)
      initializationStatus = initializePageMode(pendingInitialTouchSelection_);
    else
      initializeDirectMode();
  }

  {
    RenderLock lock(*this);
    const DictionaryFontActivation activation =
        sdFontSystem.activateDictionaryFont(renderer, dictionaryFontFamilyName_.data(), dictionaryFontPointSize_);
    definitionFontId_ = activation.fontId != 0 ? activation.fontId : SETTINGS.getBuiltInReaderFontId();
    dictionaryFontActive_ = activation.usingDictionaryFont;
  }

  if (initializationStatus != DictionaryStatus::Found) {
    if (pageMode_)
      flow_.beginPage(openedAtMs_, 0, false, 0);
    else
      flow_.beginDirect(openedAtMs_);
    flow_.onInitializationFailed(initializationStatus);
    publishRenderSnapshot();
    return;
  }

  if (pendingInitialTouchSelection_) {
    resolvePendingInitialTouch();
    if (initialTouchMiss_ && dismissOnInitialTouchMiss_) {
      finishLookup(true);
      return;
    }
  }

  publishRenderSnapshot(false);
  executeFlowCommands();
  publishRenderSnapshot();
}

void EpubReaderWordLookupActivity::saveCompleteScanCache() {
  if (!pageMode_ || !cacheIdentityValid_ || scanCachePath_[0] == '\0') return;
  const PageTextSourceView source = pageSource_.view();
  const PageWordScanCacheIdentity identity{
      cacheBackend_, spineIndex_, pageIndex_, source.contentHash, cacheDictionarySignature_, source.glyphCount};
  const uint16_t count = cacheLoaded_ ? scanCache_.candidateCount() : scanner_.candidateCount();
  const bool cursorValid = count == 0 ? flow_.cursor() == 0 : flow_.cursor() < count;
  const bool shouldSave =
      !pageSource_.truncated() && (cacheLoaded_ || (scanner_.completedSuccessfully() && !scanner_.truncated()));
  bool saved = false;
  if (shouldSave && cursorValid) {
    saved = cacheLoaded_ ? scanCache_.saveLoaded(scanCachePath_.data(), identity, flow_.cursor())
                         : scanCache_.save(scanCachePath_.data(), identity, scanner_, flow_.cursor());
  }
  if (shouldSave && (!cursorValid || !saved)) LOG_ERR("WLA", "Could not save complete wlscan cache");
}

void EpubReaderWordLookupActivity::releaseOwnedState(const bool renderLockAlreadyHeld) {
  pendingSuggestions_ = {};
  pendingResult_ = {};
  activeResult_ = {};
  lookupText_.reset();
  replacementLookupText_.reset();
  dictionarySwitchWord_.reset();
  bookReading_.reset();
  definitionModel_.clear();
  definitionBackChain_.clear();
  scanCache_.clear();
  scanner_.clear();
  pageSource_.clear();
  page_.reset();
  dictionaryOverridePath_.reset();
  completedGeneration_.store(0, std::memory_order_release);

  if (renderLockAlreadyHeld) {
    sdFontSystem.restoreReaderFont(renderer);
  } else {
    RenderLock lock(*this);
    sdFontSystem.restoreReaderFont(renderer);
  }
  dictionaryFontActive_ = false;
  shutdownComplete_ = true;
  LOG_INF("WLA", "Lookup exit heap free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void EpubReaderWordLookupActivity::shutdownBeforeFinish() {
  if (shutdownComplete_) return;
  exiting_ = true;
  flow_.beginExit();

  {
    RenderLock lock(*this);
    renderDisabled_ = true;
    renderSnapshot_.state = DictionaryLookupFlowState::Cancelled;
  }

  // Cancellation is claimed before any owned state is released. No render lock
  // is held across the worker wait or SD operations.
  engine_.cancel();
  definitionModel_.cancel();
  DictionaryLookupWorker::instance().waitForOwner(this);
  flow_.onWorkerReleased();

  saveCompleteScanCache();
  engine_.close();
  engineOpen_ = false;
  releaseOwnedState(/*renderLockAlreadyHeld=*/false);
}

void EpubReaderWordLookupActivity::onExit() {
  // ActivityManager holds RenderLock here. Normal activity-owned exits drain
  // before requesting a transition, but a global replacement can tear down a
  // parked lookup. Forced teardown must finish the cancel/join/save/close
  // sequence before destruction even though that rare path holds the manager
  // lock throughout.
  if (!shutdownComplete_) {
    exiting_ = true;
    flow_.beginExit();
    renderDisabled_ = true;
    renderSnapshot_.state = DictionaryLookupFlowState::Cancelled;
    LOG_INF("WLA", "Forced lookup teardown from ActivityManager lifecycle");
    engine_.cancel();
    definitionModel_.cancel();
    DictionaryLookupWorker::instance().waitForOwner(this);
    flow_.onWorkerReleased();
    saveCompleteScanCache();
    engine_.close();
    engineOpen_ = false;
    releaseOwnedState(/*renderLockAlreadyHeld=*/true);
  }
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

DictionaryStatus EpubReaderWordLookupActivity::startWorker(const WorkerJobKind job, const uint32_t generation) {
  if (job == WorkerJobKind::None) {
    LOG_ERR("WLA", "Refusing to start an invalid dictionary worker job");
    return DictionaryStatus::ReadError;
  }
  if (DictionaryLookupWorker::instance().isBusy() && !DictionaryLookupWorker::instance().owns(this)) {
    LOG_ERR("WLA", "Static dictionary worker is owned by another activity");
    return DictionaryStatus::ReadError;
  }
  workerJob_ = job;
  workerGeneration_ = generation;
  completedGeneration_.store(0, std::memory_order_release);
  completedJob_.store(static_cast<uint8_t>(WorkerJobKind::None), std::memory_order_relaxed);
  if (!DictionaryLookupWorker::instance().start({this, &EpubReaderWordLookupActivity::runWorkerJob})) {
    workerJob_ = WorkerJobKind::None;
    LOG_ERR("WLA", "Could not claim static dictionary worker");
    return DictionaryStatus::OutOfMemory;
  }
  return DictionaryStatus::Found;
}

void EpubReaderWordLookupActivity::runWorkerJob(void* context) {
  static_cast<EpubReaderWordLookupActivity*>(context)->runWorker();
}

void EpubReaderWordLookupActivity::runWorker() {
  const WorkerJobKind job = workerJob_;
  const uint32_t generation = workerGeneration_;
  DictionaryStatus status = DictionaryStatus::ReadError;
  switch (job) {
    case WorkerJobKind::Lookup:
      pendingResult_ = {};
      pendingSuggestions_ = {};
      status = engine_.lookup(
          {lookupText_.view(), 0,
           engine_.backendKind() == DictionaryBackendKind::Japanese ? DictionaryLookupMode::LongestAtOffset
                                                                    : DictionaryLookupMode::Token,
           lookupSyntheticKatakanaName_,
           lookupSyntheticKatakanaName_ && syntheticNameDefinition_ ? std::string_view(syntheticNameDefinition_)
                                                                    : std::string_view{}},
          pendingResult_);
      if (status == DictionaryStatus::Found && lookupDisplayPrefixLength_ != 0) {
        DictionaryOwnedText displayedHeadword;
        if (!displayedHeadword.assignJoined(std::string_view(lookupDisplayPrefix_.data(), lookupDisplayPrefixLength_),
                                            pendingResult_.headword.view())) {
          LOG_ERR("WLA", "OOM composing Japanese counter display headword");
          status = DictionaryStatus::OutOfMemory;
        } else {
          pendingResult_.headword = std::move(displayedHeadword);
        }
      }
      if (status == DictionaryStatus::NotFound && capabilities_.suggestions) {
        const DictionaryStatus suggestionStatus = engine_.suggest(lookupText_.view(), pendingSuggestions_);
        if (suggestionStatus == DictionaryStatus::ReadError || suggestionStatus == DictionaryStatus::OutOfMemory ||
            suggestionStatus == DictionaryStatus::Cancelled) {
          status = suggestionStatus;
        }
      }
      break;
    case WorkerJobKind::CollectDefinition:
      definitionModel_.collectCodepointsOnWorker();
      status = DictionaryStatus::Found;
      break;
    case WorkerJobKind::LayoutDefinition:
      definitionModel_.layoutOnWorker();
      status = DictionaryStatus::Found;
      break;
    case WorkerJobKind::None:
      LOG_ERR("WLA", "Static dictionary worker ran without an activity job");
      break;
  }
  completedStatus_.store(status, std::memory_order_relaxed);
  completedJob_.store(static_cast<uint8_t>(job), std::memory_order_relaxed);
  completedGeneration_.store(generation, std::memory_order_release);
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  LOG_DBG("WLA", "Dictionary worker stack high-water=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
}

DictionaryStatus EpubReaderWordLookupActivity::encodeCandidateText(const PageWordCandidate& candidate,
                                                                   DictionaryOwnedText& out) {
  const PageTextSourceView source = pageSource_.view();
  if (!source.glyphs || candidate.glyphCount == 0 || candidate.firstGlyph >= source.glyphCount ||
      candidate.glyphCount > source.glyphCount - candidate.firstGlyph) {
    LOG_ERR("WLA", "Selected page candidate is outside the immutable source");
    return DictionaryStatus::ReadError;
  }
  char bytes[kLookupTextBytes]{};
  size_t used = 0;
  uint16_t start = candidate.firstGlyph;
  uint16_t count = candidate.glyphCount;
  if (engine_.backendKind() == DictionaryBackendKind::Japanese) {
    size_t prefixUsed = 0;
    while (count != 0 && isJapaneseDigit(source.glyphs[start].codepoint)) {
      if (!appendCodepoint(source.glyphs[start].codepoint, lookupDisplayPrefix_.data(), lookupDisplayPrefix_.size() - 1,
                           prefixUsed)) {
        LOG_ERR("WLA", "Japanese counter prefix exceeds %u UTF-8 bytes",
                static_cast<unsigned>(lookupDisplayPrefix_.size() - 1));
        lookupDisplayPrefixLength_ = 0;
        lookupDisplayPrefix_[0] = '\0';
        return DictionaryStatus::ReadError;
      }
      ++start;
      --count;
    }
    lookupDisplayPrefixLength_ = static_cast<uint16_t>(prefixUsed);
    lookupDisplayPrefix_[lookupDisplayPrefixLength_] = '\0';
  }
  for (uint16_t offset = 0; offset < count; ++offset) {
    const PageTextGlyph& glyph = source.glyphs[start + offset];
    if (!appendCodepoint(glyph.codepoint, bytes, sizeof(bytes) - 1, used)) {
      LOG_ERR("WLA", "Selected page candidate exceeds %u UTF-8 bytes", static_cast<unsigned>(sizeof(bytes) - 1));
      return DictionaryStatus::ReadError;
    }
  }
  if (used == 0) return DictionaryStatus::NotFound;
  lookupSyntheticKatakanaName_ = engine_.backendKind() == DictionaryBackendKind::Japanese && count != 0 &&
                                 isJapaneseKatakana(source.glyphs[start].codepoint) && candidate.matchedBytes < used;
  if (!out.assign(std::string_view(bytes, used))) {
    LOG_ERR("WLA", "OOM retaining %u-byte lookup query", static_cast<unsigned>(used));
    return DictionaryStatus::OutOfMemory;
  }
  return DictionaryStatus::Found;
}

DictionaryStatus EpubReaderWordLookupActivity::prepareLookupText(const uint16_t candidateIndex) {
  lookupText_.reset();
  lookupDisplayPrefixLength_ = 0;
  lookupDisplayPrefix_[0] = '\0';
  lookupSyntheticKatakanaName_ = false;
  DictionaryStatus status = DictionaryStatus::Found;
  if (!replacementLookupText_.empty()) {
    lookupText_ = std::move(replacementLookupText_);
  } else if (!pageMode_) {
    if (directWord_.empty() || !lookupText_.assign(directWord_)) {
      LOG_ERR("WLA", "OOM retaining direct lookup word");
      return directWord_.empty() ? DictionaryStatus::NotFound : DictionaryStatus::OutOfMemory;
    }
  } else {
    const PageWordCandidate* candidate = candidateAt(candidateIndex);
    if (!candidate) {
      LOG_ERR("WLA", "Lookup flow selected a missing page candidate");
      return DictionaryStatus::ReadError;
    }
    status = encodeCandidateText(*candidate, lookupText_);
  }
  if (status != DictionaryStatus::Found) return status;
  status = normalizeRetainedDictionaryLookupText(engine_.backendKind(), lookupText_, lookupDisplayPrefix_.data(),
                                                 lookupDisplayPrefix_.size());
  if (status != DictionaryStatus::Found) {
    LOG_ERR("WLA", "Could not normalize retained lookup query: %u", static_cast<unsigned>(status));
  }
  return status;
}

void EpubReaderWordLookupActivity::executeFlowCommands() {
  for (uint8_t pass = 0; pass < 6; ++pass) {
    const DictionaryLookupFlowCommand command = flow_.takeCommand();
    if (command.action == DictionaryLookupFlowAction::None ||
        command.action == DictionaryLookupFlowAction::ReleaseResources) {
      return;
    }
    switch (command.action) {
      case DictionaryLookupFlowAction::StartLookup: {
        publishLoadingBeforeReplacement();
        {
          RenderLock lock(*this);
          definitionModel_.clear();
          activeResult_ = {};
          bookReading_.reset();
        }
        DictionaryStatus startStatus =
            engineOpen_ ? prepareLookupText(command.candidateIndex) : DictionaryStatus::Unavailable;
        if (startStatus == DictionaryStatus::Found)
          startStatus = startWorker(WorkerJobKind::Lookup, command.generation);
        if (startStatus != DictionaryStatus::Found) flow_.onCommandFailed(command.generation, startStatus);
        break;
      }
      case DictionaryLookupFlowAction::StartDefinitionCollection: {
        {
          RenderLock lock(*this);
          const PanelLayout layout = panelLayoutLocked();
          definitionModel_.begin(engine_, activeResult_.definition, command.definitionPage, layout.contentWidth,
                                 layout.linesPerPage);
        }
        const DictionaryStatus startStatus = startWorker(WorkerJobKind::CollectDefinition, command.generation);
        if (startStatus != DictionaryStatus::Found) flow_.onCommandFailed(command.generation, startStatus);
        break;
      }
      case DictionaryLookupFlowAction::PrewarmDefinition: {
        bool ready = false;
        {
          RenderLock lock(*this);
          ready = definitionModel_.prewarmOnMain(renderer, definitionFontId_, lock);
        }
        if (ready) {
          flow_.onPrewarmFinished(command.generation, true);
        } else {
          processDefinitionCompletion(command.generation);
        }
        break;
      }
      case DictionaryLookupFlowAction::StartDefinitionLayout: {
        const DictionaryStatus startStatus = startWorker(WorkerJobKind::LayoutDefinition, command.generation);
        if (startStatus != DictionaryStatus::Found) flow_.onCommandFailed(command.generation, startStatus);
      } break;
      case DictionaryLookupFlowAction::CancelAndJoin: {
        publishLoadingBeforeReplacement();
        engine_.cancel();
        definitionModel_.cancel();
        DictionaryLookupWorker::instance().waitForOwner(this);
        completedGeneration_.store(0, std::memory_order_release);
        {
          RenderLock lock(*this);
          definitionModel_.clear();
          activeResult_ = {};
          bookReading_.reset();
        }
        const DictionaryStatus reopenStatus = reopenCancelledEngine();
        flow_.onWorkerReleased();
        if (reopenStatus != DictionaryStatus::Found) {
          const auto replacement = flow_.takeCommand();
          if (replacement.action != DictionaryLookupFlowAction::None) {
            flow_.onCommandFailed(replacement.generation, reopenStatus);
          }
        }
        break;
      }
      case DictionaryLookupFlowAction::None:
      case DictionaryLookupFlowAction::ReleaseResources:
        return;
    }
  }
  LOG_ERR("WLA", "Lookup flow emitted too many commands in one loop tick");
}

DictionaryLookupFlowDefinitionEvent EpubReaderWordLookupActivity::definitionEvent(
    const DefinitionBuildState state) const {
  switch (state) {
    case DefinitionBuildState::NeedsFontPrewarm:
      return DictionaryLookupFlowDefinitionEvent::NeedsFontPrewarm;
    case DefinitionBuildState::Ready:
      return DictionaryLookupFlowDefinitionEvent::Ready;
    case DefinitionBuildState::OutOfMemory:
      return DictionaryLookupFlowDefinitionEvent::OutOfMemory;
    case DefinitionBuildState::Cancelled:
      return DictionaryLookupFlowDefinitionEvent::Cancelled;
    case DefinitionBuildState::Idle:
    case DefinitionBuildState::CollectingCodepoints:
    case DefinitionBuildState::LayingOut:
    case DefinitionBuildState::ReadError:
      return DictionaryLookupFlowDefinitionEvent::ReadError;
  }
  return DictionaryLookupFlowDefinitionEvent::ReadError;
}

void EpubReaderWordLookupActivity::processDefinitionCompletion(const uint32_t generation) {
  const DefinitionBuildState state = definitionModel_.state();
  flow_.onDefinitionEvent(generation, definitionEvent(state), definitionModel_.totalPages(),
                          definitionModel_.publishedPage());
  if (flow_.state() != DictionaryLookupFlowState::Ready || pendingDefinitionPageRestore_ < 0) return;
  const int targetPage = std::clamp(pendingDefinitionPageRestore_, 0, flow_.definitionPageCount() - 1);
  pendingDefinitionPageRestore_ = -1;
  if (targetPage != flow_.definitionPage()) {
    flow_.moveDefinitionPage(targetPage - flow_.definitionPage());
  }
}

void EpubReaderWordLookupActivity::processWorkerCompletion() {
  if (DictionaryLookupWorker::instance().owns(this)) return;
  const uint32_t generation = completedGeneration_.exchange(0, std::memory_order_acq_rel);
  if (generation == 0) return;
  const WorkerJobKind job = static_cast<WorkerJobKind>(completedJob_.load(std::memory_order_relaxed));
  const DictionaryStatus status = completedStatus_.load(std::memory_order_relaxed);
  workerJob_ = WorkerJobKind::None;

  if (job == WorkerJobKind::Lookup) {
    if (generation != flow_.generation()) return;
    const bool recordHistory = recordLookupHistory_;
    recordLookupHistory_ = true;
    notFoundShouldRecordHistory_ = recordHistory;
    if (status != DictionaryStatus::Found) pendingDefinitionPageRestore_ = -1;
    if (status == DictionaryStatus::Found) {
      {
        RenderLock lock(*this);
        activeResult_ = std::move(pendingResult_);
      }
      DictionaryOwnedText reading;
      if (capabilities_.ruby && engine_.bookReading(activeResult_.surface.view(), reading)) {
        RenderLock lock(*this);
        bookReading_ = std::move(reading);
      }
      LookupHistory::addWordIf(bookCachePath_, lookupText_.c_str(), historyStatus(activeResult_, lookupWasSuggestion_),
                               recordHistory && !bookCachePath_.empty());
      lookupWasSuggestion_ = false;
    }
    flow_.onLookupFinished(generation, status);
    executeFlowCommands();
    publishRenderSnapshot();
    if (status == DictionaryStatus::NotFound && pendingSuggestions_.count != 0 && !suggestionsShownForGeneration_) {
      suggestionsShownForGeneration_ = true;
      openSuggestions();
    } else if (status == DictionaryStatus::NotFound && recordHistory && !notFoundHistoryRecorded_) {
      LookupHistory::addWordIf(bookCachePath_, lookupText_.c_str(), LookupHistory::Status::NotFound,
                               !bookCachePath_.empty());
      notFoundHistoryRecorded_ = true;
    }
    return;
  }

  if (job == WorkerJobKind::CollectDefinition || job == WorkerJobKind::LayoutDefinition) {
    processDefinitionCompletion(generation);
    executeFlowCommands();
    publishRenderSnapshot();
    logReadyTime();
  }
}

void EpubReaderWordLookupActivity::runScanSlice() {
  if (cacheLoaded_ || flow_.scanComplete() || flow_.scanFailed() || flow_.workerOwned() || scanner_.done()) return;
  const uint16_t beforeCount = scanner_.candidateCount();
  const bool beforeDone = scanner_.done();
  flow_.beginScanSlice(millis());
  DictionaryStatus terminal = DictionaryStatus::Found;
  while (flow_.canStepScan(millis())) {
    const DictionaryStatus status = scanner_.stepOne();
    if (status != DictionaryStatus::Found && status != DictionaryStatus::NotFound) terminal = status;
    flow_.onScanProgress(scanner_.candidateCount(), scanner_.completedSuccessfully() && !scanner_.truncated(),
                         terminal);
    if (pendingInitialTouchSelection_) {
      resolvePendingInitialTouch();
      if (initialTouchMiss_ && dismissOnInitialTouchMiss_) {
        finishLookup(true);
        return;
      }
    }
    executeFlowCommands();
    if (flow_.workerOwned() || scanner_.done() || terminal != DictionaryStatus::Found) break;
  }
  if (beforeCount != scanner_.candidateCount() || beforeDone != scanner_.done()) {
    publishRenderSnapshot(scanner_.done() || beforeCount == 0 || flow_.waitingForNextCandidate());
  }
}

const PageWordCandidate* EpubReaderWordLookupActivity::candidateAt(const uint16_t index) const {
  return cacheLoaded_ ? scanCache_.candidate(index) : scanner_.candidate(index);
}

const PageWordCandidate* EpubReaderWordLookupActivity::selectedCandidate() const {
  return pageMode_ && flow_.hasSelection() ? candidateAt(flow_.cursor()) : nullptr;
}

void EpubReaderWordLookupActivity::updateHighlightSnapshot(RenderSnapshot& snapshot) const {
  snapshot.highlight = {};
  snapshot.highlightValid = false;
  const PageWordCandidate* candidate = selectedCandidate();
  if (!candidate) return;
  snapshot.highlightValid =
      unionPageTextGlyphBounds(pageSource_.view(), candidate->firstGlyph, candidate->glyphCount, snapshot.highlight);
}

void EpubReaderWordLookupActivity::publishRenderSnapshot(const bool requestRender) {
  {
    RenderLock lock(*this);
    const DictionaryLookupCandidatePresentation presentation =
        dictionaryLookupCandidatePresentation(pageMode_, flow_.hasSelection(), flow_.cursor(), flow_.discoveredCount());
    renderSnapshot_.state = flow_.state();
    renderSnapshot_.backend = cacheBackend_;
    renderSnapshot_.cursor = presentation.cursor;
    renderSnapshot_.discoveredCount = presentation.discoveredCount;
    renderSnapshot_.scanComplete = flow_.scanComplete();
    renderSnapshot_.selectionValid = presentation.selectionValid;
    renderSnapshot_.definitionPage = flow_.definitionPage();
    renderSnapshot_.definitionPageCount = flow_.definitionPageCount();
    updateHighlightSnapshot(renderSnapshot_);
  }
  const bool awaitingFirstDefinition = !readyTimeLogged_ && !flow_.openDeadlineReached(millis());
  if (requestRender && dictionaryLookupShouldRenderSnapshot(flow_.state(), awaitingFirstDefinition)) {
    requestUpdate();
  }
}

void EpubReaderWordLookupActivity::publishLoadingBeforeReplacement() {
  RenderLock lock(*this);
  renderSnapshot_.state = DictionaryLookupFlowState::Loading;
}

void EpubReaderWordLookupActivity::logReadyTime() {
  if (readyTimeLogged_ || flow_.state() != DictionaryLookupFlowState::Ready) return;
  readyTimeLogged_ = true;
  const uint32_t elapsed = millis() - openedAtMs_;
  if (elapsed > DictionaryLookupFlow::kOpenDeadlineMs) {
    LOG_ERR("WLA", "Dictionary first definition missed %u ms deadline: ready after %u ms",
            static_cast<unsigned>(DictionaryLookupFlow::kOpenDeadlineMs), static_cast<unsigned>(elapsed));
  } else {
    LOG_INF("WLA", "Dictionary first definition ready after %u ms", static_cast<unsigned>(elapsed));
  }
}

void EpubReaderWordLookupActivity::observeOpenDeadline(const uint32_t nowMs) {
  if (openDeadlineLogged_ || !flow_.openDeadlineReached(nowMs)) return;
  openDeadlineLogged_ = true;
  if (flow_.state() == DictionaryLookupFlowState::Ready) return;
  LOG_ERR("WLA", "Dictionary initial burst reached %u ms before a definition was ready",
          static_cast<unsigned>(DictionaryLookupFlow::kOpenDeadlineMs));
  requestUpdate();
}

bool EpubReaderWordLookupActivity::skipLoopDelay() {
  return !exiting_ &&
         (DictionaryLookupWorker::instance().owns(this) || flow_.workerOwned() || flow_.initialBurstActive(millis()));
}

void EpubReaderWordLookupActivity::finishLookup(const bool cancelled) {
  shutdownBeforeFinish();
  ActivityResult result;
  result.isCancelled = cancelled;
  setResult(std::move(result));
  finish();
}

void EpubReaderWordLookupActivity::returnCurrentClipping() {
  const PageWordCandidate* candidate = selectedCandidate();
  if (!candidate) return;
  const PageTextSourceView source = pageSource_.view();
  if (!source.glyphs) return;
  const uint16_t end = static_cast<uint16_t>(candidate->firstGlyph + candidate->glyphCount);
  uint16_t firstGlyph = candidate->firstGlyph;
  while (firstGlyph < end && source.glyphs[firstGlyph].pageWord == PageTextGlyph::kSyntheticPageWord) ++firstGlyph;
  uint16_t lastGlyph = end;
  while (lastGlyph > firstGlyph && source.glyphs[lastGlyph - 1].pageWord == PageTextGlyph::kSyntheticPageWord)
    --lastGlyph;
  if (firstGlyph >= lastGlyph) return;
  --lastGlyph;

  const uint16_t firstPageWord = source.glyphs[firstGlyph].pageWord;
  const uint16_t lastPageWord = source.glyphs[lastGlyph].pageWord;
  size_t firstOffset = 0;
  for (uint16_t index = firstGlyph; index > 0 && source.glyphs[index - 1].pageWord == firstPageWord; --index) {
    firstOffset += utf8Length(source.glyphs[index - 1].codepoint);
  }
  size_t lastEnd = 0;
  uint16_t lastStart = lastGlyph;
  while (lastStart > 0 && source.glyphs[lastStart - 1].pageWord == lastPageWord) --lastStart;
  for (uint16_t index = lastStart; index <= lastGlyph; ++index) lastEnd += utf8Length(source.glyphs[index].codepoint);
  if (firstOffset > UINT16_MAX || lastEnd > UINT16_MAX) {
    LOG_ERR("WLA", "Selected clipping byte range exceeds persisted limits");
    return;
  }

  DictionaryClippingRequest clipping;
  clipping.firstPageWordOrdinal = firstPageWord;
  clipping.lastPageWordOrdinal = lastPageWord;
  clipping.firstWordByteOffset = static_cast<uint16_t>(firstOffset);
  clipping.lastWordByteEndOffset = static_cast<uint16_t>(lastEnd);
  shutdownBeforeFinish();
  setResult(ActivityResult{clipping});
  finish();
}

bool EpubReaderWordLookupActivity::restartCurrentLookup(const std::string_view word, const bool suggestion,
                                                        const bool recordHistory, const bool pushDefinitionBack) {
  if (word.empty()) return false;
  if (!replacementLookupText_.assign(word)) {
    LOG_ERR("WLA", "OOM retaining replacement lookup word");
    flow_.onCommandFailed(flow_.generation(), DictionaryStatus::OutOfMemory);
    publishRenderSnapshot();
    return false;
  }
  const bool pushedBack = pushDefinitionBack;
  if (pushedBack && !definitionBackChain_.push(lookupText_.view(), flow_.definitionPage())) {
    replacementLookupText_.reset();
    LOG_ERR("WLA", "OOM retaining bounded definition lookup back entry");
    flow_.onCommandFailed(flow_.generation(), DictionaryStatus::OutOfMemory);
    publishRenderSnapshot();
    return false;
  }
  {
    RenderLock lock(*this);
    clearDefinitionSelection();
  }
  lookupWasSuggestion_ = suggestion;
  recordLookupHistory_ = recordHistory;
  notFoundHistoryRecorded_ = false;
  suggestionsShownForGeneration_ = false;
  publishLoadingBeforeReplacement();
  if (!flow_.replaceCurrentLookup()) {
    replacementLookupText_.reset();
    if (pushedBack) {
      DictionaryOwnedText discarded;
      uint16_t discardedPage = 0;
      definitionBackChain_.pop(discarded, discardedPage);
    }
    return false;
  }
  executeFlowCommands();
  publishRenderSnapshot();
  return true;
}

void EpubReaderWordLookupActivity::resetDefinitionBackChain() {
  definitionBackChain_.clear();
  pendingDefinitionPageRestore_ = -1;
}

bool EpubReaderWordLookupActivity::returnToPreviousDefinition() {
  std::string_view previousWord;
  uint16_t previousPage = 0;
  if (!definitionBackChain_.top(previousWord, previousPage)) return false;
  if (!restartCurrentLookup(previousWord, false, /*recordHistory=*/false,
                            /*pushDefinitionBack=*/false)) {
    LOG_ERR("WLA", "Could not restore prior nested dictionary lookup");
    return true;
  }
  definitionBackChain_.discardTop();
  pendingDefinitionPageRestore_ = previousPage;
  return true;
}

void EpubReaderWordLookupActivity::openSuggestions() {
  auto child = makeUniqueNoThrow<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(pendingSuggestions_));
  if (!child) {
    LOG_ERR("WLA", "OOM allocating DictionarySuggestionsActivity (%u bytes)",
            static_cast<unsigned>(sizeof(DictionarySuggestionsActivity)));
    return;
  }
  // A child can be replaced globally while this activity is parked on the
  // stack. Close the session first so its onExit fallback never waits or does
  // SD I/O while ActivityManager owns RenderLock.
  engine_.close();
  engineOpen_ = false;
  saveCompleteScanCache();
  startActivityForResult(std::move(child), [this](const ActivityResult& result) {
    {
      RenderLock lock(*this);
      initialRender_ = true;
      framebufferContainsPage_ = false;
    }
    const DictionaryStatus reopenStatus = openEngine();
    if (reopenStatus != DictionaryStatus::Found) {
      LOG_ERR("WLA", "Could not reopen dictionary after suggestions: %u", static_cast<unsigned>(reopenStatus));
      if (flow_.replaceCurrentLookup()) {
        const DictionaryLookupFlowCommand command = flow_.takeCommand();
        flow_.onCommandFailed(command.generation, reopenStatus);
      }
      publishRenderSnapshot();
      return;
    }
    if (result.isCancelled) {
      if (notFoundShouldRecordHistory_ && !notFoundHistoryRecorded_) {
        LookupHistory::addWordIf(bookCachePath_, lookupText_.c_str(), LookupHistory::Status::NotFound,
                                 !bookCachePath_.empty());
        notFoundHistoryRecorded_ = true;
      }
      requestUpdate();
      return;
    }
    const auto* selected = std::get_if<WordResult>(&result.data);
    if (!selected) {
      LOG_ERR("WLA", "Dictionary suggestions returned no word");
      requestUpdate();
      return;
    }
    restartCurrentLookup(selected->word, true);
  });
}

void EpubReaderWordLookupActivity::openDictionarySwitcher() {
  if (!capabilities_.dictionarySwitch || engine_.backendKind() != DictionaryBackendKind::StarDict ||
      flow_.workerOwned()) {
    return;
  }
  auto child = makeUniqueNoThrow<DictionarySelectActivity>(renderer, mappedInput, bookCachePath_, true, true);
  if (!child) {
    LOG_ERR("WLA", "OOM allocating DictionarySelectActivity (%u bytes)",
            static_cast<unsigned>(sizeof(DictionarySelectActivity)));
    return;
  }
  if (!dictionarySwitchWord_.assign(lookupText_.view())) {
    LOG_ERR("WLA", "OOM retaining lookup word across dictionary selection");
    flow_.onCommandFailed(flow_.generation(), DictionaryStatus::OutOfMemory);
    publishRenderSnapshot();
    return;
  }
  {
    RenderLock lock(*this);
    if (pageMode_ && renderSnapshot_.highlightValid) {
      initialTouchX_ = renderSnapshot_.highlight.x + renderSnapshot_.highlight.width / 2;
      initialTouchY_ = renderSnapshot_.highlight.y + renderSnapshot_.highlight.height / 2;
    }
  }
  // SdFat permits only one reader on hardware. Release the active backend
  // before the child enumerates dictionary metadata.
  engine_.close();
  engineOpen_ = false;
  saveCompleteScanCache();
  startActivityForResult(std::move(child), [this](const ActivityResult& result) {
    {
      RenderLock lock(*this);
      initialRender_ = true;
      framebufferContainsPage_ = false;
    }
    const auto publishOpenFailure = [this](const DictionaryStatus status) {
      if (flow_.replaceCurrentLookup()) {
        const DictionaryLookupFlowCommand command = flow_.takeCommand();
        flow_.onCommandFailed(command.generation, status);
      }
      publishRenderSnapshot();
    };
    if (result.isCancelled) {
      const DictionaryStatus reopenStatus = openEngine();
      if (reopenStatus == DictionaryStatus::Found)
        restartCurrentLookup(dictionarySwitchWord_.view(), false, /*recordHistory=*/false);
      else
        publishOpenFailure(reopenStatus);
      return;
    }
    const auto* selected = std::get_if<FilePathResult>(&result.data);
    if (!selected || selected->path.empty()) {
      LOG_ERR("WLA", "Dictionary switch returned no path");
      const DictionaryStatus reopenStatus = openEngine();
      if (reopenStatus == DictionaryStatus::Found)
        restartCurrentLookup(dictionarySwitchWord_.view(), false, /*recordHistory=*/false);
      else
        publishOpenFailure(reopenStatus);
      return;
    }
    publishLoadingBeforeReplacement();
    DictionaryOwnedText selectedOverride;
    if (!selectedOverride.assign(selected->path)) {
      LOG_ERR("WLA", "OOM retaining selected dictionary path");
      const DictionaryStatus restoreStatus = openEngine();
      if (restoreStatus != DictionaryStatus::Found)
        LOG_ERR("WLA", "Could not restore previous dictionary after picker OOM: %u",
                static_cast<unsigned>(restoreStatus));
      publishOpenFailure(DictionaryStatus::OutOfMemory);
      return;
    }
    DictionaryOwnedText previousOverride = std::move(dictionaryOverridePath_);
    dictionaryOverridePath_ = std::move(selectedOverride);
    const DictionaryStatus openStatus = openEngine();
    if (openStatus != DictionaryStatus::Found) {
      dictionaryOverridePath_ = std::move(previousOverride);
      const DictionaryStatus restoreStatus = openEngine();
      if (restoreStatus != DictionaryStatus::Found)
        LOG_ERR("WLA", "Could not restore previous dictionary after selection failure: %u",
                static_cast<unsigned>(restoreStatus));
      publishOpenFailure(openStatus);
      return;
    }
    {
      RenderLock lock(*this);
      definitionModel_.clear();
      activeResult_ = {};
      bookReading_.reset();
      clearDefinitionSelection();
    }
    if (!pageMode_) {
      restartCurrentLookup(dictionarySwitchWord_.view(), false, /*recordHistory=*/false);
      return;
    }

    cacheLoaded_ = false;
    scanCache_.clear();
    scanner_.clear();
    if (!replacementLookupText_.assign(dictionarySwitchWord_.view())) {
      LOG_ERR("WLA", "OOM retaining current lookup across page dictionary rescan");
      publishOpenFailure(DictionaryStatus::OutOfMemory);
      return;
    }
    pendingInitialTouchSelection_ = initialTouchX_ >= 0 && initialTouchY_ >= 0;
    dismissOnInitialTouchMiss_ = false;
    nearestOnInitialTouchMiss_ = false;
    initialTouchMiss_ = false;
    recordLookupHistory_ = false;
    const DictionaryStatus initializationStatus = initializePageMode(pendingInitialTouchSelection_);
    if (initializationStatus != DictionaryStatus::Found) {
      flow_.onInitializationFailed(initializationStatus);
      publishRenderSnapshot();
      return;
    }
    if (pendingInitialTouchSelection_) resolvePendingInitialTouch();
    executeFlowCommands();
    publishRenderSnapshot();
  });
}

uint16_t EpubReaderWordLookupActivity::candidateAtPoint(const int x, const int y, const bool exactOnly) const {
  uint16_t best = UINT16_MAX;
  int64_t bestDistance = std::numeric_limits<int64_t>::max();
  const uint16_t count = cacheLoaded_ ? scanCache_.candidateCount() : scanner_.candidateCount();
  for (uint16_t index = 0; index < count; ++index) {
    const PageWordCandidate* candidate = candidateAt(index);
    PageTextBounds bounds;
    if (!candidate ||
        !unionPageTextGlyphBounds(pageSource_.view(), candidate->firstGlyph, candidate->glyphCount, bounds)) {
      continue;
    }
    if (x >= bounds.x && x < bounds.x + bounds.width && y >= bounds.y && y < bounds.y + bounds.height) return index;
    if (exactOnly) continue;
    const int64_t centerX = bounds.x + bounds.width / 2;
    const int64_t centerY = bounds.y + bounds.height / 2;
    const int64_t dx = centerX - x;
    const int64_t dy = centerY - y;
    const int64_t distance = dx * dx + dy * dy;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = index;
    }
  }
  return best;
}

uint16_t EpubReaderWordLookupActivity::nearestCandidateAt(const int x, const int y) const {
  return candidateAtPoint(x, y, false);
}

bool EpubReaderWordLookupActivity::resolvePendingInitialTouch() {
  if (!pendingInitialTouchSelection_) return true;
  const uint16_t exact = candidateAtPoint(initialTouchX_, initialTouchY_, true);
  if (exact != UINT16_MAX && flow_.selectInitialCandidate(exact)) {
    pendingInitialTouchSelection_ = false;
    initialTouchMiss_ = false;
    return true;
  }

  const PageTextSourceView source = pageSource_.view();
  uint16_t touchedGlyph = UINT16_MAX;
  for (uint16_t index = 0; index < source.glyphCount; ++index) {
    const auto& glyph = source.glyphs[index];
    if (initialTouchX_ >= glyph.x && initialTouchX_ < glyph.x + glyph.width && initialTouchY_ >= glyph.y &&
        initialTouchY_ < glyph.y + glyph.height) {
      touchedGlyph = index;
      break;
    }
  }
  const bool scanComplete = cacheLoaded_ || scanner_.done();
  const bool touchedGlyphProcessed =
      touchedGlyph != UINT16_MAX && (cacheLoaded_ || scanner_.hasProcessedGlyph(touchedGlyph));
  const bool conclusive = dictionaryLookupInitialTouchMissIsConclusive(
      dismissOnInitialTouchMiss_, touchedGlyph != UINT16_MAX, touchedGlyphProcessed, scanComplete);
  if (!conclusive) return false;

  pendingInitialTouchSelection_ = false;
  initialTouchMiss_ = true;
  if (nearestOnInitialTouchMiss_) {
    const uint16_t nearest = nearestCandidateAt(initialTouchX_, initialTouchY_);
    if (nearest != UINT16_MAX && flow_.selectInitialCandidate(nearest)) initialTouchMiss_ = false;
  }
  if (initialTouchMiss_ && !dismissOnInitialTouchMiss_) flow_.onInitializationFailed(DictionaryStatus::NotFound);
  return !initialTouchMiss_;
}

int EpubReaderWordLookupActivity::definitionFontId(const bool isIpa) const {
  // CrossInk currently uses the active dictionary/reader family for IPA too,
  // but keep the published IPA bit in the selection and rendering path. This
  // keeps measurement identical to Task 12's immutable-page geometry.
  (void)isIpa;
  return definitionFontId_;
}

int EpubReaderWordLookupActivity::measureDefinitionText(const std::string_view text, const EpdFontFamily::Style style,
                                                        const bool isIpa) const {
  int total = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t codepoint = 0;
    size_t length = 0;
    if (!decodeUtf8(text, offset, codepoint, length)) return -1;
    char encoded[5]{};
    size_t used = 0;
    if (!appendCodepoint(codepoint, encoded, sizeof(encoded) - 1, used)) return -1;
    encoded[used] = '\0';
    const int width = renderer.getTextAdvanceX(definitionFontId(isIpa), encoded, style);
    if (width < 0 || total > std::numeric_limits<int>::max() - width) return -1;
    total += width;
    offset += length;
  }
  return total;
}

bool EpubReaderWordLookupActivity::findDefinitionToken(const uint16_t wantedIndex, const int touchX, const int touchY,
                                                       const bool useTouch, DefinitionToken& selected,
                                                       uint16_t& tokenCount) const {
  selected = {};
  tokenCount = 0;
  if (flow_.state() != DictionaryLookupFlowState::Ready || definitionModel_.state() != DefinitionBuildState::Ready) {
    return false;
  }
  const PanelLayout layout = panelLayoutLocked();
  const DictionaryDefinitionPage& page = definitionModel_.page();
  const int indentWidth = measureDefinitionText("   ", EpdFontFamily::REGULAR);
  const int bulletWidth = measureDefinitionText(kBullet, EpdFontFamily::REGULAR);
  if (indentWidth < 0 || bulletWidth < 0) return false;

  for (uint16_t lineIndex = 0; lineIndex < page.lineCount; ++lineIndex) {
    const DictionaryDefinitionPageLine& line = page.lines[lineIndex];
    const int y = layout.bodyY + lineIndex * layout.lineHeight;
    if (y + layout.lineHeight > layout.bodyBottom) break;
    int x = layout.contentX + line.indentLevel * indentWidth + (line.isListItem ? bulletWidth : 0);
    for (uint16_t segmentOffset = 0; segmentOffset < line.segmentCount; ++segmentOffset) {
      const uint32_t segmentIndex = line.firstSegment + segmentOffset;
      if (segmentIndex >= page.segmentCount) return selected.valid;
      const auto& segment = page.segments[segmentIndex];
      const std::string_view text = page.segmentText(segmentIndex);
      size_t offset = 0;
      while (offset < text.size()) {
        uint32_t codepoint = 0;
        size_t length = 0;
        if (!decodeUtf8(text, offset, codepoint, length)) return selected.valid;
        if (definitionTokenDelimiter(codepoint)) {
          const int advance = measureDefinitionText(text.substr(offset, length), segment.style, segment.isIpa);
          if (advance < 0) return selected.valid;
          x += advance;
          offset += length;
          continue;
        }

        const size_t tokenStart = offset;
        const int tokenX = x;
        size_t tokenBytes = 0;
        int tokenWidth = 0;
        while (offset < text.size()) {
          if (!decodeUtf8(text, offset, codepoint, length) || definitionTokenDelimiter(codepoint)) break;
          if (tokenBytes != 0 && tokenBytes + length >= kLookupTextBytes) break;
          const int advance = measureDefinitionText(text.substr(offset, length), segment.style, segment.isIpa);
          if (advance < 0) return selected.valid;
          tokenBytes += length;
          tokenWidth += advance;
          x += advance;
          offset += length;
        }
        if (tokenBytes == 0) return selected.valid;
        const bool touched = useTouch && touchX >= tokenX && touchX < tokenX + tokenWidth && touchY >= y &&
                             touchY < y + layout.lineHeight;
        if (!selected.valid && (touched || (!useTouch && tokenCount == wantedIndex))) {
          selected.text = text.substr(tokenStart, tokenBytes);
          selected.bounds = {static_cast<int16_t>(tokenX), static_cast<int16_t>(y),
                             static_cast<int16_t>(std::min(tokenWidth, static_cast<int>(INT16_MAX))),
                             static_cast<int16_t>(std::min(layout.lineHeight, static_cast<int>(INT16_MAX)))};
          selected.index = tokenCount;
          selected.valid = true;
        }
        if (tokenCount != UINT16_MAX) ++tokenCount;
      }
    }
  }
  return selected.valid;
}

void EpubReaderWordLookupActivity::clearDefinitionSelection() {
  definitionSelectionMode_ = false;
  definitionMultiSelectMode_ = false;
  definitionConfirmReleaseConsumed_ = false;
  definitionTouchDragLookup_ = false;
  definitionTokenIndex_ = 0;
  definitionTokenCount_ = 0;
  definitionSelectionAnchor_ = 0;
  renderSnapshot_.definitionHighlight = {};
  renderSnapshot_.definitionHighlightValid = false;
}

bool EpubReaderWordLookupActivity::enterDefinitionSelection() {
  DefinitionToken token;
  {
    RenderLock lock(*this);
    uint16_t count = 0;
    if (!findDefinitionToken(0, 0, 0, false, token, count)) return false;
    definitionSelectionMode_ = true;
    definitionTokenIndex_ = token.index;
    definitionTokenCount_ = count;
    definitionSelectionAnchor_ = token.index;
    renderSnapshot_.definitionHighlight = token.bounds;
    renderSnapshot_.definitionHighlightValid = true;
  }
  requestUpdate();
  return true;
}

#if CROSSINK_APP_CAP_TOUCH
bool EpubReaderWordLookupActivity::selectDefinitionTokenAt(const int x, const int y, const bool beginTouchDrag) {
  DefinitionToken token;
  {
    RenderLock lock(*this);
    uint16_t count = 0;
    if (!findDefinitionToken(0, x, y, true, token, count)) return false;
    definitionSelectionMode_ = true;
    definitionMultiSelectMode_ = beginTouchDrag;
    definitionTouchDragLookup_ = beginTouchDrag;
    definitionConfirmReleaseConsumed_ = false;
    definitionTokenIndex_ = token.index;
    definitionTokenCount_ = count;
    definitionSelectionAnchor_ = token.index;
    renderSnapshot_.definitionHighlight = token.bounds;
    renderSnapshot_.definitionHighlightValid = true;
  }
  requestUpdate();
  return true;
}
#endif

bool EpubReaderWordLookupActivity::moveDefinitionSelection(const int delta) {
  if (!definitionSelectionMode_ || definitionTokenCount_ == 0 || delta == 0) return false;
  const int requested =
      std::clamp<int>(static_cast<int>(definitionTokenIndex_) + delta, 0, static_cast<int>(definitionTokenCount_) - 1);
  if (requested == definitionTokenIndex_) return false;
  DefinitionToken token;
  {
    RenderLock lock(*this);
    uint16_t count = 0;
    if (!findDefinitionToken(static_cast<uint16_t>(requested), 0, 0, false, token, count)) return false;
    definitionTokenIndex_ = token.index;
    definitionTokenCount_ = count;
    renderSnapshot_.definitionHighlight = token.bounds;
    renderSnapshot_.definitionHighlightValid = true;
  }
  requestUpdate();
  return true;
}

bool EpubReaderWordLookupActivity::lookupDefinitionSelection() {
  if (!definitionSelectionMode_) return false;
  DefinitionToken token;
  {
    RenderLock lock(*this);
    uint16_t count = 0;
    if (!findDefinitionToken(definitionTokenIndex_, 0, 0, false, token, count)) return false;
  }
  return restartCurrentLookup(token.text, false, /*recordHistory=*/true, /*pushDefinitionBack=*/true);
}

bool EpubReaderWordLookupActivity::lookupDefinitionSelectionRange() {
  if (!definitionSelectionMode_) return false;
  const uint16_t first = std::min(definitionSelectionAnchor_, definitionTokenIndex_);
  const uint16_t last = std::max(definitionSelectionAnchor_, definitionTokenIndex_);
  char phrase[kLookupTextBytes]{};
  size_t used = 0;
  for (uint16_t index = first;; ++index) {
    DefinitionToken token;
    {
      RenderLock lock(*this);
      uint16_t count = 0;
      if (!findDefinitionToken(index, 0, 0, false, token, count)) return false;
    }
    const size_t separator = used == 0 ? 0 : 1;
    if (token.text.size() > sizeof(phrase) - 1 - used - separator) {
      LOG_ERR("WLA", "Definition multi-word lookup exceeds %u UTF-8 bytes", static_cast<unsigned>(sizeof(phrase) - 1));
      flow_.onCommandFailed(flow_.generation(), DictionaryStatus::ReadError);
      publishRenderSnapshot();
      return false;
    }
    if (separator != 0) phrase[used++] = ' ';
    std::memcpy(phrase + used, token.text.data(), token.text.size());
    used += token.text.size();
    if (index == last) break;
  }
  phrase[used] = '\0';
  return restartCurrentLookup(std::string_view(phrase, used), false, /*recordHistory=*/true,
                              /*pushDefinitionBack=*/true);
}

#if CROSSINK_APP_CAP_TOUCH
bool EpubReaderWordLookupActivity::lookupDefinitionTokenAt(const int x, const int y) {
  DefinitionToken token;
  {
    RenderLock lock(*this);
    uint16_t count = 0;
    if (!findDefinitionToken(0, x, y, true, token, count)) return false;
  }
  return restartCurrentLookup(token.text, false, /*recordHistory=*/true, /*pushDefinitionBack=*/true);
}
#endif

void EpubReaderWordLookupActivity::loop() {
  processWorkerCompletion();
  if (exiting_) return;
  observeOpenDeadline(millis());

  if (ignoreInitialBackRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      ignoreInitialBackRelease_ = false;
    }
    return;
  }

  if (exitAllOnBackRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      finishLookup(false);
    }
    return;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= kLongPressMs) {
    exitAllOnBackRelease_ = true;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (definitionSelectionMode_) {
      {
        RenderLock lock(*this);
        if (definitionMultiSelectMode_) {
          definitionMultiSelectMode_ = false;
          definitionTouchDragLookup_ = false;
          definitionSelectionAnchor_ = definitionTokenIndex_;
        } else {
          clearDefinitionSelection();
        }
      }
      requestUpdate();
      return;
    }
    if (returnToPreviousDefinition()) return;
    finishLookup(true);
    return;
  }
  if (dictionaryLookupPowerReleaseDismisses(SETTINGS.shortPwrBtn,
                                            static_cast<uint8_t>(CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD),
                                            mappedInput.wasReleased(MappedInputManager::Button::Power),
                                            mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    finishLookup(true);
    return;
  }

  if (!definitionSelectionMode_ && capabilities_.dictionarySwitch &&
      mappedInput.isPressed(MappedInputManager::Button::Left) && mappedInput.getHeldTime() >= kLongPressMs) {
    if (!dictionarySwitchHeld_) {
      dictionarySwitchHeld_ = true;
      openDictionarySwitcher();
    }
    return;
  }
  if (dictionarySwitchHeld_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Left)) dictionarySwitchHeld_ = false;
    return;
  }

  if (definitionSelectionMode_) {
    if (definitionConfirmReleaseConsumed_) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        definitionConfirmReleaseConsumed_ = false;
      }
      return;
    }
    if (!definitionMultiSelectMode_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= kLongPressMs) {
      {
        RenderLock lock(*this);
        definitionMultiSelectMode_ = true;
        definitionSelectionAnchor_ = definitionTokenIndex_;
        definitionConfirmReleaseConsumed_ = true;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      moveDefinitionSelection(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      moveDefinitionSelection(1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (definitionMultiSelectMode_)
        lookupDefinitionSelectionRange();
      else
        lookupDefinitionSelection();
      return;
    }
  }

  const bool sideButtonsForLookup = dictionaryLookupUsesSideButtons(
      SETTINGS.wordLookupSideButtons, SETTINGS.sideButtonLayout, CrossPointSettings::SIDE_BUTTONS_DISABLED);
  const bool previousEntryTriggered = sideButtonsForLookup ? DictUtils::dictionaryPageButtonTriggered(mappedInput, true)
                                                           : mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextEntryTriggered = sideButtonsForLookup ? DictUtils::dictionaryPageButtonTriggered(mappedInput, false)
                                                       : mappedInput.wasReleased(MappedInputManager::Button::Right);
  const DictionaryLookupScrollButtons scrollButtons =
      dictionaryLookupScrollButtons(sideButtonsForLookup, mappedInput.isFrontNavButtonSwapActive());
  const auto scrollDownButton = lookupNavigationButton(scrollButtons.down);
  const auto scrollUpButton = lookupNavigationButton(scrollButtons.up);

  if (previousEntryTriggered) {
    if (flow_.moveCursor(-1)) {
      resetDefinitionBackChain();
      recordLookupHistory_ = true;
      {
        RenderLock lock(*this);
        clearDefinitionSelection();
      }
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }
  if (nextEntryTriggered) {
    if (flow_.moveCursor(1)) {
      resetDefinitionBackChain();
      recordLookupHistory_ = true;
      {
        RenderLock lock(*this);
        clearDefinitionSelection();
      }
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }
  if (mappedInput.wasReleased(scrollUpButton)) {
    if (flow_.moveDefinitionPage(-1)) {
      {
        RenderLock lock(*this);
        clearDefinitionSelection();
      }
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }
  if (mappedInput.wasReleased(scrollDownButton)) {
    if (flow_.moveDefinitionPage(1)) {
      {
        RenderLock lock(*this);
        clearDefinitionSelection();
      }
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kLongPressMs) {
    mappedInput.suppressNextConfirmRelease();
    if (pageMode_)
      returnCurrentClipping();
    else
      openDictionarySwitcher();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    enterDefinitionSelection();
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  const auto swipe = definitionTouchDragLookup_ ? MappedInputManager::SwipeDir::None : mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
    const int delta = swipe == MappedInputManager::SwipeDir::Left ? 1 : -1;
    if (definitionSelectionMode_) {
      moveDefinitionSelection(delta);
      return;
    }
    if (flow_.moveCursor(delta)) {
      resetDefinitionBackChain();
      recordLookupHistory_ = true;
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;
    if (definitionSelectionMode_) {
      moveDefinitionSelection(delta);
      return;
    }
    if (flow_.moveDefinitionPage(delta)) {
      executeFlowCommands();
      publishRenderSnapshot();
    }
    return;
  }

  if (definitionTouchDragLookup_) {
    int dragX = 0;
    int dragY = 0;
    if (mappedInput.isScreenTouchHeld(dragX, dragY)) {
      DefinitionToken token;
      uint16_t count = 0;
      bool changed = false;
      {
        RenderLock lock(*this);
        if (findDefinitionToken(0, dragX, dragY, true, token, count) && token.index != definitionTokenIndex_) {
          definitionTokenIndex_ = token.index;
          definitionTokenCount_ = count;
          renderSnapshot_.definitionHighlight = token.bounds;
          renderSnapshot_.definitionHighlightValid = true;
          changed = true;
        }
      }
      if (changed) requestUpdate();
      return;
    }
    if (mappedInput.wasScreenTouchReleased()) {
      definitionTouchDragLookup_ = false;
      mappedInput.suppressNextTouchTap();
      lookupDefinitionSelectionRange();
      return;
    }
  }

  int touchDownX = 0;
  int touchDownY = 0;
  if (mappedInput.wasScreenTouchDown(touchDownX, touchDownY)) {
    if (selectDefinitionTokenAt(touchDownX, touchDownY, /*beginTouchDrag=*/true)) {
      return;
    }
  }

  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    bool insidePanel = false;
    bool insideFooter = false;
    bool insideBody = false;
    bool insideRightHalf = false;
    {
      RenderLock lock(*this);
      const PanelLayout layout = panelLayoutLocked();
      insidePanel = panelContainsLocked(touchX, touchY);
      insideFooter = touchY >= layout.panel.y + layout.panel.height - layout.lineHeight * 2;
      insideBody = touchY >= layout.bodyY && touchY < layout.bodyBottom;
      insideRightHalf = touchX >= layout.panel.x + layout.panel.width / 2;
    }
    if (!insidePanel) {
      finishLookup(true);
      return;
    }
    if (insideFooter) {
      if (pageMode_ && insideRightHalf) {
        returnCurrentClipping();
      } else if (capabilities_.dictionarySwitch) {
        openDictionarySwitcher();
      }
      return;
    }
    if (insideBody && lookupDefinitionTokenAt(touchX, touchY)) return;
  }
#endif

  // Input is always polled before this bounded main-task scan slice.
  runScanSlice();
  processWorkerCompletion();
}

EpubReaderWordLookupActivity::PanelLayout EpubReaderWordLookupActivity::panelLayoutLocked() const {
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, true, false);
  int viewTop = 0;
  int viewRight = 0;
  int viewBottom = 0;
  int viewLeft = 0;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);

  const int sideMargin = std::max(metrics.optionPopupDialogSideMargin, std::max(viewLeft, viewRight));
  const int topMargin = std::max(metrics.optionPopupDialogSideMargin, viewTop);
  const int bottomMargin = std::max(metrics.optionPopupDialogSideMargin, viewBottom);
  const int availableWidth = std::max(1, safe.width - sideMargin * 2);
  const int availableHeight = std::max(1, safe.height - topMargin - bottomMargin);
  const int desiredHeight = std::max(availableHeight / 2, (availableHeight * 2) / 3);
  const int panelHeight = std::clamp(desiredHeight, 1, availableHeight);

  PanelLayout layout;
  layout.panel =
      Rect{safe.x + sideMargin, safe.y + safe.height - bottomMargin - panelHeight, availableWidth, panelHeight};
  const int inner = metrics.optionPopupInnerPadding;
  layout.contentX = layout.panel.x + inner;
  layout.contentWidth = std::max(1, layout.panel.width - inner * 2);
  layout.lineHeight = std::max(1, renderer.getLineHeight(definitionFontId_));
  const int headerHeight = renderer.getLineHeight(UI_10_FONT_ID);
  layout.titleY = layout.panel.y + inner + headerHeight + metrics.optionPopupTitleGap;
  int metadataRows = 1;
  if (activeResult_.transformed && !activeResult_.surface.empty()) ++metadataRows;
  if (!activeResult_.reading.empty()) ++metadataRows;
  if (!bookReading_.empty()) ++metadataRows;
  layout.bodyY = layout.titleY + metadataRows * layout.lineHeight + metrics.optionPopupTitleGap;
  layout.bodyBottom = layout.panel.y + layout.panel.height - inner - layout.lineHeight - metrics.optionPopupTitleGap;
  layout.linesPerPage = std::max(1, (layout.bodyBottom - layout.bodyY) / layout.lineHeight);
  return layout;
}

#if CROSSINK_APP_CAP_TOUCH
bool EpubReaderWordLookupActivity::panelContainsLocked(const int x, const int y) const {
  const Rect panel = panelLayoutLocked().panel;
  return x >= panel.x && x < panel.x + panel.width && y >= panel.y && y < panel.y + panel.height;
}
#endif

void EpubReaderWordLookupActivity::renderReaderBackground() {
  if (!pageMode_) {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    return;
  }
  if (initialRender_ && framebufferContainsPage_) {
    framebufferContainsPage_ = false;
    if (reservedBottomHeight_ > 0) {
      const int height = std::min(reservedBottomHeight_, renderer.getScreenHeight());
      renderer.fillRect(0, renderer.getScreenHeight() - height, renderer.getScreenWidth(), height,
                        ReaderUtils::readerForegroundBlack() ? false : true);
    }
    return;
  }

  const bool dictionaryFontWasSelected = dictionaryFontActive_;
  if (dictionaryFontWasSelected) sdFontSystem.restoreReaderFont(renderer);
  if (readerBackgroundRender_) {
    readerBackgroundRender_(readerContext_);
  } else if (page_) {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    if (auto* cache = renderer.getFontCacheManager()) {
      auto scope = cache->createPrewarmScope();
      page_->render(renderer, SETTINGS.getReaderFontId(), marginLeft_, marginTop_,
                    ReaderUtils::readerForegroundBlack());
      if (scope.endScanAndPrewarm()) {
        page_->render(renderer, SETTINGS.getReaderFontId(), marginLeft_, marginTop_,
                      ReaderUtils::readerForegroundBlack());
      }
    } else {
      page_->render(renderer, SETTINGS.getReaderFontId(), marginLeft_, marginTop_,
                    ReaderUtils::readerForegroundBlack());
    }
  }
  if (dictionaryFontWasSelected) {
    const DictionaryFontActivation activation =
        sdFontSystem.activateDictionaryFont(renderer, dictionaryFontFamilyName_.data(), dictionaryFontPointSize_);
    definitionFontId_ = activation.fontId != 0 ? activation.fontId : SETTINGS.getBuiltInReaderFontId();
    dictionaryFontActive_ = activation.usingDictionaryFont;
  }
}

void EpubReaderWordLookupActivity::drawPanelFrame(const PanelLayout& layout) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frame = std::max(1, metrics.popupFrameThickness);
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  const Color foreground = foregroundBlack ? Color::Black : Color::White;
  const Color background = foregroundBlack ? Color::White : Color::Black;
  if (metrics.popupCornerRadius > 0) {
    renderer.fillRoundedRect(layout.panel.x - frame, layout.panel.y - frame, layout.panel.width + frame * 2,
                             layout.panel.height + frame * 2, metrics.popupCornerRadius + frame, foreground);
    renderer.fillRoundedRect(layout.panel.x, layout.panel.y, layout.panel.width, layout.panel.height,
                             metrics.popupCornerRadius, background);
  } else {
    renderer.fillRect(layout.panel.x - frame, layout.panel.y - frame, layout.panel.width + frame * 2,
                      layout.panel.height + frame * 2, foregroundBlack);
    renderer.fillRect(layout.panel.x, layout.panel.y, layout.panel.width, layout.panel.height, !foregroundBlack);
  }
}

void EpubReaderWordLookupActivity::drawPanelHeader(const PanelLayout& layout, const RenderSnapshot& snapshot) const {
  char position[32] = {};
  if (snapshot.selectionValid) {
    if (snapshot.scanComplete) {
      std::snprintf(position, sizeof(position), "%u/%u", static_cast<unsigned>(snapshot.cursor + 1),
                    static_cast<unsigned>(snapshot.discoveredCount));
    } else {
      std::snprintf(position, sizeof(position), "%u/%s", static_cast<unsigned>(snapshot.cursor + 1), kEllipsis);
    }
  }
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  renderer.drawText(UI_10_FONT_ID, layout.contentX, layout.panel.y + 3, tr(STR_LOOKUP), foregroundBlack,
                    EpdFontFamily::BOLD);
  if (position[0] != '\0') {
    const int width = renderer.getTextWidth(UI_10_FONT_ID, position);
    renderer.drawText(UI_10_FONT_ID, layout.panel.x + layout.panel.width - width - 4, layout.panel.y + 3, position,
                      foregroundBlack);
  }
}

void EpubReaderWordLookupActivity::drawLoadingOrError(const PanelLayout& layout,
                                                      const DictionaryLookupFlowState state) const {
  const char* message = tr(STR_LOADING);
  if (state == DictionaryLookupFlowState::NotFound) message = tr(STR_NOT_FOUND);
  if (state == DictionaryLookupFlowState::Unavailable) message = tr(STR_DICT_NO_DICT_SET);
  if (state == DictionaryLookupFlowState::ReadError) message = tr(STR_DICT_READ_FAILED);
  if (state == DictionaryLookupFlowState::OutOfMemory) message = tr(STR_MEMORY_ERROR);
  UITheme::drawCenteredText(renderer, layout.panel, UI_10_FONT_ID,
                            layout.panel.y + (layout.panel.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, message,
                            ReaderUtils::readerForegroundBlack());
}

void EpubReaderWordLookupActivity::drawDefinitionPass(const PanelLayout& layout) const {
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  int titleY = layout.titleY;
  renderer.beginTextClip(layout.contentX, layout.panel.y, layout.contentWidth, layout.panel.height);
  renderer.drawText(definitionFontId_, layout.contentX, titleY, activeResult_.headword.c_str(), foregroundBlack,
                    EpdFontFamily::BOLD);
  titleY += layout.lineHeight;
  if (activeResult_.transformed && !activeResult_.surface.empty()) {
    renderer.drawText(definitionFontId_, layout.contentX, titleY, "~ ", foregroundBlack);
    const int markerWidth = renderer.getTextAdvanceX(definitionFontId_, "~ ", EpdFontFamily::REGULAR);
    renderer.drawText(definitionFontId_, layout.contentX + markerWidth, titleY, activeResult_.surface.c_str(),
                      foregroundBlack);
    titleY += layout.lineHeight;
  }
  if (!activeResult_.reading.empty()) {
    renderer.drawText(definitionFontId_, layout.contentX, titleY, activeResult_.reading.c_str(), foregroundBlack);
    titleY += layout.lineHeight;
  }
  if (!bookReading_.empty()) {
    renderer.drawText(UI_10_FONT_ID, layout.contentX, titleY, tr(STR_IN_THIS_BOOK), foregroundBlack,
                      EpdFontFamily::BOLD);
    const int labelWidth = renderer.getTextAdvanceX(UI_10_FONT_ID, tr(STR_IN_THIS_BOOK), EpdFontFamily::BOLD);
    renderer.drawText(definitionFontId_, layout.contentX + labelWidth + 4, titleY, bookReading_.c_str(),
                      foregroundBlack);
  }

  const DictionaryDefinitionPage& page = definitionModel_.page();
  const int indentWidth = renderer.getTextAdvanceX(definitionFontId_, "   ", EpdFontFamily::REGULAR);
  const int bulletWidth = renderer.getTextAdvanceX(definitionFontId_, kBullet, EpdFontFamily::REGULAR);
  for (uint16_t lineIndex = 0; lineIndex < page.lineCount; ++lineIndex) {
    const DictionaryDefinitionPageLine& line = page.lines[lineIndex];
    const int y = layout.bodyY + lineIndex * layout.lineHeight;
    if (y + layout.lineHeight > layout.bodyBottom) break;
    int x = layout.contentX + line.indentLevel * indentWidth;
    if (line.isListItem) {
      renderer.drawText(definitionFontId_, x, y, kBullet, foregroundBlack);
      x += bulletWidth;
    }
    for (uint16_t segmentOffset = 0; segmentOffset < line.segmentCount; ++segmentOffset) {
      const uint32_t segmentIndex = line.firstSegment + segmentOffset;
      if (segmentIndex >= page.segmentCount) break;
      const DictionaryDefinitionPageSegment& segment = page.segments[segmentIndex];
      const char* text = page.textPool + segment.textOffset;
      int baselineY = y;
      if ((segment.style & EpdFontFamily::SUP) != 0) baselineY -= layout.lineHeight / 4;
      if ((segment.style & EpdFontFamily::SUB) != 0) baselineY += layout.lineHeight / 4;
      const int segmentFontId = definitionFontId(segment.isIpa);
      renderer.drawText(segmentFontId, x, baselineY, text, foregroundBlack, segment.style);
      const int width = renderer.getTextAdvanceX(segmentFontId, text, segment.style);
      if ((segment.style & EpdFontFamily::UNDERLINE) != 0) {
        renderer.drawLine(x, y + renderer.getFontAscenderSize(segmentFontId) + 2, x + width,
                          y + renderer.getFontAscenderSize(segmentFontId) + 2, foregroundBlack);
      }
      if ((segment.style & EpdFontFamily::STRIKETHROUGH) != 0) {
        renderer.drawLine(x, y + renderer.getFontAscenderSize(segmentFontId) / 2, x + width,
                          y + renderer.getFontAscenderSize(segmentFontId) / 2, foregroundBlack);
      }
      x += width;
    }
  }
  renderer.endTextClip();
}

void EpubReaderWordLookupActivity::drawDefinition(const PanelLayout& layout) const {
  if (renderer.isSdCardFont(definitionFontId_)) {
    if (auto* cache = renderer.getFontCacheManager()) {
      auto scope = cache->createPrewarmScope(FontCacheManager::PreparationPolicy::DictionaryLean);
      drawDefinitionPass(layout);
      if (scope.endScanAndPrewarm()) {
        drawDefinitionPass(layout);
        return;
      }
      LOG_ERR("WLA", "Could not prewarm the visible dictionary definition font");
    }
  }
  drawDefinitionPass(layout);
}

void EpubReaderWordLookupActivity::drawButtonHints() const {
  const bool sideButtonsForLookup = dictionaryLookupUsesSideButtons(
      SETTINGS.wordLookupSideButtons, SETTINGS.sideButtonLayout, CrossPointSettings::SIDE_BUTTONS_DISABLED);
  const DictionaryLookupScrollButtons scrollButtons =
      dictionaryLookupScrollButtons(sideButtonsForLookup, mappedInput.isFrontNavButtonSwapActive());
  const bool logicalLeftScrollsUp = scrollButtons.up == DictionaryLookupNavigationButton::Left;
  const char* sideLeftLabel = logicalLeftScrollsUp ? tr(STR_DIR_UP) : tr(STR_DIR_DOWN);
  const char* sideRightLabel = logicalLeftScrollsUp ? tr(STR_DIR_DOWN) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            definitionMultiSelectMode_ ? tr(STR_DONE) : tr(STR_LOOKUP_SHORT),
                                            sideButtonsForLookup ? sideLeftLabel : tr(STR_DIR_LEFT),
                                            sideButtonsForLookup ? sideRightLabel : tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderWordLookupActivity::displayPanelRefresh(const bool framebufferContainedPage) {
  if (initialRender_) {
    const DictionaryLookupPanelRefresh refresh = dictionaryLookupInitialPanelRefresh(framebufferContainedPage, pageMode_);
    renderer.displayBuffer(refresh == DictionaryLookupPanelRefresh::Fast ? HalDisplay::FAST_REFRESH
                                                                         : HalDisplay::FULL_REFRESH);
    initialRender_ = false;
    fastRefreshCount_ = refresh == DictionaryLookupPanelRefresh::Fast ? 1 : 0;
    return;
  }
  ++fastRefreshCount_;
  if (fastRefreshCount_ >= kCleanupRefreshInterval) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    fastRefreshCount_ = 0;
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  if (renderDisabled_) return;
  const RenderSnapshot snapshot = renderSnapshot_;
  // ActivityManager also requests an initial render, independently of snapshot
  // publication. Defer that loading frame even when the word scan was cached.
  if (initialRender_ && !dictionaryLookupShouldRenderSnapshot(
                            snapshot.state, static_cast<uint32_t>(millis() - openedAtMs_) <
                                                DictionaryLookupFlow::kOpenDeadlineMs)) {
    return;
  }
  const bool framebufferContainedPage = initialRender_ && framebufferContainsPage_;
  renderReaderBackground();
  if (snapshot.highlightValid) {
    renderer.invertRect(snapshot.highlight.x - 2, snapshot.highlight.y - 2, snapshot.highlight.width + 4,
                        snapshot.highlight.height + 4);
  }

  const PanelLayout layout = panelLayoutLocked();
  drawPanelFrame(layout);
  drawPanelHeader(layout, snapshot);
  if (snapshot.state == DictionaryLookupFlowState::Ready && definitionModel_.state() == DefinitionBuildState::Ready) {
    drawDefinition(layout);
    if (snapshot.definitionHighlightValid) {
      renderer.invertRect(snapshot.definitionHighlight.x - 1, snapshot.definitionHighlight.y - 1,
                          snapshot.definitionHighlight.width + 2, snapshot.definitionHighlight.height + 2);
    }
  } else {
    const DictionaryLookupFlowState visibleState =
        snapshot.state == DictionaryLookupFlowState::NotFound && !snapshot.scanComplete
            ? DictionaryLookupFlowState::Loading
            : snapshot.state;
    drawLoadingOrError(layout, visibleState);
  }

  const int footerY = layout.panel.y + layout.panel.height - layout.lineHeight - 2;
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  renderer.drawLine(layout.panel.x, footerY - 2, layout.panel.x + layout.panel.width, footerY - 2, foregroundBlack);
  const char* source =
      snapshot.backend == DictionaryBackendKind::Japanese ? tr(STR_DICT_EFFECTIVE_JAPANESE) : tr(STR_DICTIONARY);
  renderer.drawText(UI_10_FONT_ID, layout.contentX, footerY, source, foregroundBlack);
  drawButtonHints();
  displayPanelRefresh(framebufferContainedPage);
}
