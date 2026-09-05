#pragma once

#include <DictHtmlRenderer.h>
#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IpaUtils.h"  // IpaTextSpan (Wrapper scratch member)

// Pure, renderer-independent layout of dictionary HTML spans into wrapped
// display lines. Width measurement is injected via Measurer, so this module has
// no dependency on GfxRenderer / CrossPointSettings / font IDs. That decoupling
// is what lets the wrap/pagination logic be unit-tested host-side with a
// deterministic fake measurer (Tier-A litmus), while the device supplies a
// real font-metric-backed measurer.
namespace DictLayout {

// A single styled run within a display line.
struct LayoutSegment {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool isIpa = false;  // true → render with the IPA font
};

// One wrapped display line, containing one or more styled segments.
struct LayoutLine {
  std::vector<LayoutSegment> segments;
  uint8_t indentLevel = 0;
  bool isListItem = false;
};

// A completed line borrowed from Wrapper's reusable scratch buffers. The sink
// must copy anything it needs before returning.
struct LayoutSegmentRef {
  uint16_t offset = 0;
  uint16_t length = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool isIpa = false;
};

// Length-aware input used by worker-side dictionary layout. Unlike StyledSpan,
// text is not required to be NUL-terminated and may end in the middle of a
// UTF-8 sequence; BoundedWrapper carries that sequence into the next span.
struct LengthSpan {
  std::string_view text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool ipa = false;
  bool isListItem = false;
  bool newlineBefore = false;
  uint8_t indentLevel = 0;
};

// Length-safe width callback for borrowed streaming spans. The callback must
// not retain text. Function-pointer + context keeps the worker path allocation
// free and avoids std::function on the 4-KiB task stack.
struct LengthMeasurer {
  void* ctx = nullptr;
  int (*fn)(void* ctx, std::string_view text, EpdFontFamily::Style style, bool isIpa) = nullptr;
  int operator()(std::string_view text, EpdFontFamily::Style style, bool isIpa) const {
    return fn ? fn(ctx, text, style, isIpa) : -1;
  }
};
struct LayoutLineView {
  const char* textPool = nullptr;
  const LayoutSegmentRef* segments = nullptr;
  uint16_t segmentCount = 0;
  uint8_t indentLevel = 0;
  bool isListItem = false;
};

// Width measurement injection. fn returns the pixel width of `text` rendered at
// `style`, using the IPA font when isIpa is true and the body font otherwise.
// Function-pointer + ctx (NOT std::function) per the project's binary-size rules.
struct Measurer {
  void* ctx = nullptr;
  int (*fn)(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa) = nullptr;
  int operator()(const char* text, EpdFontFamily::Style style, bool isIpa) const { return fn(ctx, text, style, isIpa); }
};

// Pixel metrics for one wrap pass. maxWidth is the usable line width; indentStep
// is one indent level in px; bulletWidth is the list-item bullet prefix width.
struct WrapMetrics {
  int maxWidth = 0;
  int indentStep = 0;
  int bulletWidth = 0;
};

// Receives each completed display line as it is produced, in order. The callee
// may keep or discard each line (e.g. retain only one page's worth while counting
// the rest) — wrapSpans always produces every line exactly once. Function-pointer
// + ctx (NOT std::function) per the project's binary-size rules.
struct LineSink {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const LayoutLineView& line) = nullptr;
  void operator()(const LayoutLineView& line) const { fn(ctx, line); }
};

// A cancellable sink for the bounded worker wrapper. Returning false stops
// before any later line is emitted.
struct ControlledLineSink {
  void* ctx = nullptr;
  bool (*fn)(void* ctx, const LayoutLineView& line) = nullptr;
  bool operator()(const LayoutLineView& line) const { return fn && fn(ctx, line); }
};

enum class BoundedWrapStatus : uint8_t { Ok, InvalidUtf8, Overflow, SinkRejected };

// Fixed scratch owned by the caller. Firmware allocates this once on the heap;
// keeping it out of BoundedWrapper prevents a roughly 2-KiB worker stack frame.
// The byte cap also prevents an adversarial unbroken/combining-mark token from
// turning cross-chunk carry into unbounded heap growth.
struct BoundedWrapScratch {
  static constexpr uint16_t kTextCapacity = 512;
  static constexpr uint16_t kSegmentCapacity = 64;
  char lineText[kTextCapacity]{};
  LayoutSegmentRef lineSegments[kSegmentCapacity]{};
  char tokenText[kTextCapacity]{};
  LayoutSegmentRef tokenSegments[kSegmentCapacity]{};
};

// Allocation-free, length-aware streaming wrapper for dictionary worker jobs.
// Span and UTF-8 boundaries are independent: words and up-to-four-byte UTF-8
// sequences may cross any number of source chunks. finish() rejects a trailing
// partial sequence. Scratch is reusable only after finish()/failure returns.
class BoundedWrapper {
 public:
  BoundedWrapper(const WrapMetrics& metrics, const LengthMeasurer& measure, const ControlledLineSink& sink,
                 BoundedWrapScratch& scratch);

  bool onSpan(const LengthSpan& span);
  bool finish();
  BoundedWrapStatus status() const { return status_; }

 private:
  struct SpanStyle {
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool ipa = false;
    uint8_t indentLevel = 0;
  };

  bool acceptCodepoint(const char* bytes, uint8_t length, uint32_t codepoint, const SpanStyle& spanStyle);
  bool appendToken(const char* bytes, uint8_t length, EpdFontFamily::Style style, bool isIpa, int width,
                   uint8_t indent);
  bool flushToken(bool forcedContinuation = false);
  bool appendTokenRange(uint16_t offset, uint16_t length, EpdFontFamily::Style style, bool isIpa);
  bool appendLineBytes(const char* bytes, uint16_t length, EpdFontFamily::Style style, bool isIpa, int width);
  bool flushLine();
  bool startLine(uint8_t indent, bool listItem);
  int measureCodepoint(std::string_view bytes, EpdFontFamily::Style style, bool isIpa) const;
  bool fail(BoundedWrapStatus status);

  int maxWidth_ = 0;
  int indentStep_ = 0;
  int bulletWidth_ = 0;
  LengthMeasurer measure_{};
  ControlledLineSink sink_{};
  BoundedWrapScratch& scratch_;
  BoundedWrapStatus status_ = BoundedWrapStatus::Ok;
  uint16_t lineBytes_ = 0;
  uint16_t lineSegmentCount_ = 0;
  uint16_t tokenBytes_ = 0;
  uint16_t tokenSegmentCount_ = 0;
  int currentX_ = 0;
  int tokenWidth_ = 0;
  uint8_t lineIndent_ = 0;
  uint8_t tokenIndent_ = 0;
  bool lineIsListItem_ = false;
  bool pendingSpace_ = false;
  bool continuingToken_ = false;
  char utf8Pending_[4]{};
  uint8_t utf8PendingLength_ = 0;
  uint8_t utf8ExpectedLength_ = 0;
  SpanStyle utf8PendingStyle_{};
};

// Stateful word-wrapper. Spans are fed one at a time via onSpan() (so the source
// — the HTML renderer — can stream them and never materialize the whole
// definition), and each completed line is emitted to the LineSink as it is
// finished. finish() flushes the trailing line. Equivalent to the one-shot
// wrapSpans(); that overload is a thin loop over this class.
class Wrapper {
 public:
  Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink);

  void onSpan(const StyledSpan& span);                        // process one span; emit completed lines to the sink
  void lineBreak(uint8_t indent = 0, bool listItem = false);  // flush and begin a new logical line
  void finish();                                              // flush the trailing in-progress line

 private:
  int getMixedWidth(const char* text, EpdFontFamily::Style style);
  void flushLine();
  void startLine(uint8_t indent, bool listItem);
  void appendToLine(const std::string& text, EpdFontFamily::Style style, bool isIpa, int width);
  void appendMixed(const char* text, EpdFontFamily::Style style);
  void breakToken(const std::string& tok, EpdFontFamily::Style style, uint8_t indentLevel);

  const int maxWidth_;
  const int indentStep_;
  const int bulletWidth_;
  Measurer measure_;
  LineSink sink_;
  std::string lineTextPool_;
  std::vector<LayoutSegmentRef> lineSegments_;
  uint8_t lineIndent_ = 0;
  bool lineIsListItem_ = false;
  int currentX_ = 0;
  bool pendingInterSpanSpace_ = false;
  std::vector<IpaTextSpan> ipaRuns_;  // reused scratch for IPA run splitting
};

// Streaming layout: word-wrap every span and emit each completed line to `sink`
// as soon as it is finished. Lets the caller hold only a subset of lines (one
// page) rather than the whole definition — this is the Stage 2a RAM fix.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink);

// Reference full-layout path: word-wrap every span into `out` (cleared first).
// Produces the entire definition's lines in one pass — the differential oracle
// the streaming path is checked against. Implemented on top of the sink variant.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out);

// Number of pages for a given line count and page size. Always >= 1.
inline int paginate(int lineCount, int linesPerPage) {
  if (linesPerPage < 1) linesPerPage = 1;
  const int pages = (lineCount + linesPerPage - 1) / linesPerPage;
  return pages < 1 ? 1 : pages;
}

}  // namespace DictLayout
