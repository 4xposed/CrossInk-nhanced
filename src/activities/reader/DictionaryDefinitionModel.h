#pragma once

#include <EpdFontFamily.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "util/DictLayout.h"
#include "util/DictionaryEngine.h"

class GfxRenderer;
class RenderLock;

enum class DefinitionBuildState : uint8_t {
  Idle,
  CollectingCodepoints,
  NeedsFontPrewarm,
  LayingOut,
  Ready,
  ReadError,
  OutOfMemory,
  Cancelled,
};

struct DefinitionAdvanceTable {
  static constexpr uint16_t kCodepointCapacity = 256;
  std::array<uint32_t, kCodepointCapacity> codepoints{};
  std::array<std::array<int16_t, 4>, kCodepointCapacity> advances{};
  uint16_t count = 0;
};

struct DictionaryDefinitionPageSegment {
  uint32_t textOffset = 0;
  uint32_t textLength = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool isIpa = false;
};

struct DictionaryDefinitionPageLine {
  uint32_t firstSegment = 0;
  uint16_t segmentCount = 0;
  uint8_t indentLevel = 0;
  bool isListItem = false;
};

// Immutable after publication. The public views are const even though the
// backing exact-size arrays remain exclusively owned by this value.
class DictionaryDefinitionPage {
 public:
  DictionaryDefinitionPage() = default;
  DictionaryDefinitionPage(DictionaryDefinitionPage&&) noexcept = default;
  DictionaryDefinitionPage& operator=(DictionaryDefinitionPage&&) noexcept = default;
  DictionaryDefinitionPage(const DictionaryDefinitionPage&) = delete;
  DictionaryDefinitionPage& operator=(const DictionaryDefinitionPage&) = delete;

  std::string_view segmentText(uint32_t index) const;

  const DictionaryDefinitionPageLine* lines = nullptr;
  const DictionaryDefinitionPageSegment* segments = nullptr;
  const char* textPool = nullptr;
  uint16_t lineCount = 0;
  uint32_t segmentCount = 0;
  uint32_t textPoolBytes = 0;
  int pageIndex = 0;

 private:
  friend class DictionaryDefinitionModel;
  void clear();

  std::unique_ptr<DictionaryDefinitionPageLine[]> ownedLines_;
  std::unique_ptr<DictionaryDefinitionPageSegment[]> ownedSegments_;
  std::unique_ptr<char[]> ownedTextPool_;
};

class DictionaryDefinitionModel {
 public:
  DictionaryDefinitionModel() = default;
  // The owner must cancel and wait for the static dictionary worker before
  // clear() or destruction; neither operation is safe while a callback owns us.
  ~DictionaryDefinitionModel() { clear(); }
  DictionaryDefinitionModel(const DictionaryDefinitionModel&) = delete;
  DictionaryDefinitionModel& operator=(const DictionaryDefinitionModel&) = delete;

  void begin(DictionaryEngine& engine, DictionaryDefinitionHandle handle, int targetPage, int maxWidth,
             int linesPerPage);
  void collectCodepointsOnWorker();
  bool prewarmOnMain(GfxRenderer& renderer, int fontId, RenderLock& lock);
  void layoutOnWorker();

  DefinitionBuildState state() const { return state_.load(std::memory_order_acquire); }
  const DictionaryDefinitionPage& page() const { return page_; }
  int totalPages() const { return totalPages_; }
  int publishedPage() const { return page_.pageIndex; }
  const DefinitionAdvanceTable& advanceTable() const { return advanceTable_; }
  bool codepointTableTruncated() const { return codepointTableTruncated_; }

  void cancel();
  void clear();

 private:
  struct CollectionDecoder {
    char pending[4]{};
    uint8_t pendingLength = 0;
    uint8_t expectedLength = 0;
  };

  bool collectSpan(const DictionaryDefinitionSpan& span, CollectionDecoder& decoder);
  bool collectBytes(std::string_view text, CollectionDecoder& decoder);
  void retainCodepoint(uint32_t codepoint);
  void publishTerminal(DefinitionBuildState state, const char* message);
  static EpdFontFamily::Style styleFor(const DictionaryDefinitionSpan& span);
  static int measureFromTable(void* context, std::string_view text, EpdFontFamily::Style style, bool isIpa);

  std::atomic<DefinitionBuildState> state_{DefinitionBuildState::Idle};
  std::atomic_bool cancelRequested_{false};
  DictionaryEngine* engine_ = nullptr;
  DictionaryDefinitionHandle handle_{};
  int requestedPage_ = 0;
  int maxWidth_ = 0;
  int linesPerPage_ = 1;
  int totalPages_ = 0;
  int fontId_ = 0;
  int indentStep_ = 0;
  int bulletWidth_ = 0;
  uint64_t collectedSourceHash_ = 0;
  uint32_t collectedSourceUnitCount_ = 0;
  uint8_t styleMask_ = 0;
  DefinitionAdvanceTable advanceTable_{};
  std::array<int16_t, 4> narrowFallbackAdvances_{};
  std::array<int16_t, 4> wideFallbackAdvances_{};
  bool codepointTableTruncated_ = false;
  bool fallbackWidthUsed_ = false;
  bool plainFallback_ = false;
  std::unique_ptr<DictLayout::BoundedWrapScratch> layoutScratch_;
  DictionaryDefinitionPage page_;
};
