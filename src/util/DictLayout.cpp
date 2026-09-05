#include "DictLayout.h"

#include <Utf8.h>

#include <climits>
#include <cstring>
#include <numeric>
#include <utility>

#include "IpaUtils.h"

namespace DictLayout {

namespace {

bool decodeCompleteUtf8(const char* bytes, const uint8_t length, uint32_t& codepoint) {
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

uint8_t expectedUtf8Length(const uint8_t lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

}  // namespace

BoundedWrapper::BoundedWrapper(const WrapMetrics& metrics, const LengthMeasurer& measure,
                               const ControlledLineSink& sink, BoundedWrapScratch& scratch)
    : maxWidth_(metrics.maxWidth),
      indentStep_(metrics.indentStep),
      bulletWidth_(metrics.bulletWidth),
      measure_(measure),
      sink_(sink),
      scratch_(scratch) {
  if (maxWidth_ <= 0 || indentStep_ < 0 || bulletWidth_ < 0 || !measure_.fn || !sink_.fn) {
    status_ = BoundedWrapStatus::Overflow;
    return;
  }
  startLine(0, false);
}

bool BoundedWrapper::fail(const BoundedWrapStatus status) {
  if (status_ == BoundedWrapStatus::Ok) status_ = status;
  return false;
}

bool BoundedWrapper::startLine(const uint8_t indent, const bool listItem) {
  const int64_t initial = static_cast<int64_t>(indent) * indentStep_ + (listItem ? bulletWidth_ : 0);
  if (initial < 0 || initial > INT_MAX) return fail(BoundedWrapStatus::Overflow);
  lineIndent_ = indent;
  lineIsListItem_ = listItem;
  currentX_ = static_cast<int>(initial);
  pendingSpace_ = false;
  return true;
}

int BoundedWrapper::measureCodepoint(const std::string_view bytes, const EpdFontFamily::Style style,
                                     const bool isIpa) const {
  const auto baseStyle = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(style) & 0x03U);
  int width = measure_(bytes, baseStyle, isIpa);
  if (width < 0) return -1;
  if ((style & EpdFontFamily::SUP) != 0 || (style & EpdFontFamily::SUB) != 0) width = (width + 1) / 2;
  return width;
}

bool BoundedWrapper::flushLine() {
  if (lineSegmentCount_ == 0) return true;
  if (!sink_({scratch_.lineText, scratch_.lineSegments, lineSegmentCount_, lineIndent_, lineIsListItem_})) {
    return fail(BoundedWrapStatus::SinkRejected);
  }
  lineBytes_ = 0;
  lineSegmentCount_ = 0;
  pendingSpace_ = false;
  return true;
}

bool BoundedWrapper::appendLineBytes(const char* bytes, const uint16_t length, const EpdFontFamily::Style style,
                                     const bool isIpa, const int width) {
  if (length == 0) return true;
  if (width < 0 || currentX_ > INT_MAX - width) return fail(BoundedWrapStatus::Overflow);
  const bool merges = lineSegmentCount_ != 0 && scratch_.lineSegments[lineSegmentCount_ - 1].style == style &&
                      scratch_.lineSegments[lineSegmentCount_ - 1].isIpa == isIpa;
  if (lineBytes_ > BoundedWrapScratch::kTextCapacity - length ||
      (!merges && lineSegmentCount_ == BoundedWrapScratch::kSegmentCapacity)) {
    if (!flushLine() || !startLine(tokenIndent_, false)) return false;
  }
  const bool mergeAfterFlush = lineSegmentCount_ != 0 && scratch_.lineSegments[lineSegmentCount_ - 1].style == style &&
                               scratch_.lineSegments[lineSegmentCount_ - 1].isIpa == isIpa;
  if (lineBytes_ > BoundedWrapScratch::kTextCapacity - length ||
      (!mergeAfterFlush && lineSegmentCount_ == BoundedWrapScratch::kSegmentCapacity)) {
    return fail(BoundedWrapStatus::Overflow);
  }
  if (mergeAfterFlush) {
    auto& segment = scratch_.lineSegments[lineSegmentCount_ - 1];
    if (segment.length > UINT16_MAX - length) return fail(BoundedWrapStatus::Overflow);
    segment.length = static_cast<uint16_t>(segment.length + length);
  } else {
    scratch_.lineSegments[lineSegmentCount_++] = {lineBytes_, length, style, isIpa};
  }
  std::memcpy(scratch_.lineText + lineBytes_, bytes, length);
  lineBytes_ = static_cast<uint16_t>(lineBytes_ + length);
  currentX_ += width;
  return true;
}

bool BoundedWrapper::appendTokenRange(const uint16_t offset, const uint16_t length, const EpdFontFamily::Style style,
                                      const bool isIpa) {
  uint16_t consumed = 0;
  while (consumed < length) {
    const uint8_t lead = static_cast<uint8_t>(scratch_.tokenText[offset + consumed]);
    const uint8_t cpLength = expectedUtf8Length(lead);
    if (cpLength == 0 || cpLength > length - consumed) return fail(BoundedWrapStatus::InvalidUtf8);
    uint32_t codepoint = 0;
    if (!decodeCompleteUtf8(scratch_.tokenText + offset + consumed, cpLength, codepoint)) {
      return fail(BoundedWrapStatus::InvalidUtf8);
    }
    (void)codepoint;
    const std::string_view bytes(scratch_.tokenText + offset + consumed, cpLength);
    const int width = measureCodepoint(bytes, style, isIpa);
    if (width < 0) return fail(BoundedWrapStatus::Overflow);
    if (currentX_ > INT_MAX - width) return fail(BoundedWrapStatus::Overflow);
    if (currentX_ + width > maxWidth_ && lineSegmentCount_ != 0) {
      if (!flushLine() || !startLine(tokenIndent_, false)) return false;
    }
    if (!appendLineBytes(bytes.data(), cpLength, style, isIpa, width)) return false;
    consumed = static_cast<uint16_t>(consumed + cpLength);
  }
  return true;
}

bool BoundedWrapper::flushToken(const bool forcedContinuation) {
  if (tokenBytes_ == 0) {
    continuingToken_ = forcedContinuation;
    return true;
  }

  bool useSpace = !continuingToken_ && pendingSpace_ && lineSegmentCount_ != 0;
  int spaceWidth = 0;
  if (useSpace) {
    const auto style = scratch_.tokenSegments[0].style;
    spaceWidth = measureCodepoint(std::string_view(" ", 1), style, false);
    if (spaceWidth < 0) return fail(BoundedWrapStatus::Overflow);

    bool capacityFits = lineBytes_ < BoundedWrapScratch::kTextCapacity &&
                        tokenBytes_ <= BoundedWrapScratch::kTextCapacity - lineBytes_ - 1U;
    uint16_t projectedSegments = lineSegmentCount_;
    EpdFontFamily::Style projectedStyle = scratch_.lineSegments[lineSegmentCount_ - 1].style;
    bool projectedIpa = scratch_.lineSegments[lineSegmentCount_ - 1].isIpa;
    const auto addProjectedRun = [&](const EpdFontFamily::Style nextStyle, const bool nextIpa) {
      if (projectedStyle == nextStyle && projectedIpa == nextIpa) return true;
      if (projectedSegments == BoundedWrapScratch::kSegmentCapacity) return false;
      ++projectedSegments;
      projectedStyle = nextStyle;
      projectedIpa = nextIpa;
      return true;
    };
    capacityFits = capacityFits && addProjectedRun(style, false);
    for (uint16_t index = 0; capacityFits && index < tokenSegmentCount_; ++index) {
      const auto& segment = scratch_.tokenSegments[index];
      capacityFits = addProjectedRun(segment.style, segment.isIpa);
    }
    if (!capacityFits) {
      if (!flushLine() || !startLine(tokenIndent_, false)) return false;
      useSpace = false;
      spaceWidth = 0;
    }
  }
  if (currentX_ > INT_MAX - spaceWidth || currentX_ + spaceWidth > INT_MAX - tokenWidth_) {
    return fail(BoundedWrapStatus::Overflow);
  }
  if (currentX_ + spaceWidth + tokenWidth_ > maxWidth_ && lineSegmentCount_ != 0) {
    if (!flushLine() || !startLine(tokenIndent_, false)) return false;
    useSpace = false;
  }
  if (useSpace && !appendLineBytes(" ", 1, scratch_.tokenSegments[0].style, false, spaceWidth)) return false;

  for (uint16_t index = 0; index < tokenSegmentCount_; ++index) {
    const auto segment = scratch_.tokenSegments[index];
    if (segment.offset > tokenBytes_ || segment.length > tokenBytes_ - segment.offset ||
        !appendTokenRange(segment.offset, segment.length, segment.style, segment.isIpa)) {
      return false;
    }
  }
  tokenBytes_ = 0;
  tokenSegmentCount_ = 0;
  tokenWidth_ = 0;
  pendingSpace_ = false;
  continuingToken_ = forcedContinuation;
  return true;
}

bool BoundedWrapper::appendToken(const char* bytes, const uint8_t length, const EpdFontFamily::Style style,
                                 const bool isIpa, const int width, const uint8_t indent) {
  if (length == 0) return fail(BoundedWrapStatus::Overflow);
  bool merges = tokenSegmentCount_ != 0 && scratch_.tokenSegments[tokenSegmentCount_ - 1].style == style &&
                scratch_.tokenSegments[tokenSegmentCount_ - 1].isIpa == isIpa;
  if (tokenBytes_ > BoundedWrapScratch::kTextCapacity - length ||
      (!merges && tokenSegmentCount_ == BoundedWrapScratch::kSegmentCapacity)) {
    if (!flushToken(true)) return false;
    merges = false;
  }
  if (tokenBytes_ == 0) tokenIndent_ = indent;
  if (tokenBytes_ > BoundedWrapScratch::kTextCapacity - length ||
      (!merges && tokenSegmentCount_ == BoundedWrapScratch::kSegmentCapacity) || width < 0 ||
      tokenWidth_ > INT_MAX - width) {
    return fail(BoundedWrapStatus::Overflow);
  }
  if (merges) {
    auto& segment = scratch_.tokenSegments[tokenSegmentCount_ - 1];
    if (segment.length > UINT16_MAX - length) return fail(BoundedWrapStatus::Overflow);
    segment.length = static_cast<uint16_t>(segment.length + length);
  } else {
    scratch_.tokenSegments[tokenSegmentCount_++] = {tokenBytes_, length, style, isIpa};
  }
  std::memcpy(scratch_.tokenText + tokenBytes_, bytes, length);
  tokenBytes_ = static_cast<uint16_t>(tokenBytes_ + length);
  tokenWidth_ += width;
  return true;
}

bool BoundedWrapper::acceptCodepoint(const char* bytes, const uint8_t length, const uint32_t codepoint,
                                     const SpanStyle& spanStyle) {
  if (codepoint == 0 || codepoint == ' ' || codepoint == '\t') {
    if (!flushToken()) return false;
    continuingToken_ = false;
    pendingSpace_ = lineSegmentCount_ != 0;
    return true;
  }
  if (codepoint == '\r' || codepoint == '\n') {
    if (!flushToken() || !flushLine() || !startLine(spanStyle.indentLevel, false)) return false;
    continuingToken_ = false;
    return true;
  }
  const bool combining = utf8IsCombiningMark(codepoint);
  const bool priorIpa = tokenSegmentCount_ != 0 && scratch_.tokenSegments[tokenSegmentCount_ - 1].isIpa;
  const bool isIpa = spanStyle.ipa || (combining ? priorIpa : isIpaCodepoint(codepoint));
  const int width = measureCodepoint(std::string_view(bytes, length), spanStyle.style, isIpa);
  return appendToken(bytes, length, spanStyle.style, isIpa, width, spanStyle.indentLevel);
}

bool BoundedWrapper::onSpan(const LengthSpan& span) {
  if (status_ != BoundedWrapStatus::Ok) return false;
  if (span.newlineBefore) {
    if (utf8PendingLength_ != 0) return fail(BoundedWrapStatus::InvalidUtf8);
    if (!flushToken() || !flushLine() || !startLine(span.indentLevel, span.isListItem)) return false;
    continuingToken_ = false;
  } else if (lineSegmentCount_ == 0 && tokenBytes_ == 0 && !pendingSpace_ &&
             (span.indentLevel != lineIndent_ || span.isListItem != lineIsListItem_)) {
    if (!startLine(span.indentLevel, span.isListItem)) return false;
  }

  const SpanStyle style{span.style, span.ipa, span.indentLevel};
  for (const char value : span.text) {
    const uint8_t byte = static_cast<uint8_t>(value);
    if (utf8PendingLength_ == 0) {
      const uint8_t expected = expectedUtf8Length(byte);
      if (expected == 0) return fail(BoundedWrapStatus::InvalidUtf8);
      if (expected == 1) {
        if (!acceptCodepoint(&value, 1, byte, style)) return false;
        continue;
      }
      utf8Pending_[0] = value;
      utf8PendingLength_ = 1;
      utf8ExpectedLength_ = expected;
      utf8PendingStyle_ = style;
      continue;
    }
    if ((byte & 0xC0U) != 0x80U || utf8PendingLength_ >= sizeof(utf8Pending_)) {
      return fail(BoundedWrapStatus::InvalidUtf8);
    }
    utf8Pending_[utf8PendingLength_++] = value;
    if (utf8PendingLength_ == utf8ExpectedLength_) {
      uint32_t codepoint = 0;
      if (!decodeCompleteUtf8(utf8Pending_, utf8PendingLength_, codepoint) ||
          !acceptCodepoint(utf8Pending_, utf8PendingLength_, codepoint, utf8PendingStyle_)) {
        return status_ == BoundedWrapStatus::Ok ? fail(BoundedWrapStatus::InvalidUtf8) : false;
      }
      utf8PendingLength_ = 0;
      utf8ExpectedLength_ = 0;
    }
  }
  return true;
}

bool BoundedWrapper::finish() {
  if (status_ != BoundedWrapStatus::Ok) return false;
  if (utf8PendingLength_ != 0) return fail(BoundedWrapStatus::InvalidUtf8);
  return flushToken() && flushLine();
}

static bool hasAsciiSpace(const char* text) {
  if (!text) return false;
  while (*text) {
    if (*text == ' ') return true;
    ++text;
  }
  return false;
}

Wrapper::Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink)
    : maxWidth_(metrics.maxWidth),
      indentStep_(metrics.indentStep),
      bulletWidth_(metrics.bulletWidth),
      measure_(measure),
      sink_(sink) {
  ipaRuns_.reserve(4);
  lineTextPool_.reserve(128);
  lineSegments_.reserve(4);
  startLine(0, false);
}

// Width of a string, accounting for mixed IPA/non-IPA runs (each run measured
// with the appropriate font via the injected measurer).
int Wrapper::getMixedWidth(const char* text, EpdFontFamily::Style style) {
  ipaRuns_.clear();
  splitIpaRuns(text, ipaRuns_);
  return std::accumulate(ipaRuns_.begin(), ipaRuns_.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + measure_(run.text.c_str(), style, run.isIpa);
  });
}

void Wrapper::flushLine() {
  if (!lineSegments_.empty()) {
    sink_({lineTextPool_.c_str(), lineSegments_.data(), static_cast<uint16_t>(lineSegments_.size()), lineIndent_,
           lineIsListItem_});
    lineTextPool_.clear();
    lineSegments_.clear();
  }
  pendingInterSpanSpace_ = false;
}

void Wrapper::startLine(uint8_t indent, bool listItem) {
  lineIndent_ = indent;
  lineIsListItem_ = listItem;
  currentX_ = indent * indentStep_ + (listItem ? bulletWidth_ : 0);
  pendingInterSpanSpace_ = false;
}

void Wrapper::appendToLine(const std::string& text, EpdFontFamily::Style style, bool isIpa, int width) {
  if (!lineSegments_.empty() && lineSegments_.back().style == style && lineSegments_.back().isIpa == isIpa) {
    lineSegments_.back().length = static_cast<uint16_t>(lineSegments_.back().length + text.size());
  } else {
    lineSegments_.push_back(
        {static_cast<uint16_t>(lineTextPool_.size()), static_cast<uint16_t>(text.size()), style, isIpa});
  }
  lineTextPool_ += text;
  currentX_ += width;
}

void Wrapper::appendMixed(const char* text, EpdFontFamily::Style style) {
  ipaRuns_.clear();
  splitIpaRuns(text, ipaRuns_);
  for (const auto& run : ipaRuns_) {
    appendToLine(run.text, style, run.isIpa, measure_(run.text.c_str(), style, run.isIpa));
  }
}

// Break a single token at codepoint boundaries when it is wider than the available line width.
// IPA combining-mark handling (pendingIsIpa) mirrors splitIpaRuns and must be preserved.
void Wrapper::breakToken(const std::string& tok, EpdFontFamily::Style style, uint8_t indentLevel) {
  const auto* bp = reinterpret_cast<const uint8_t*>(tok.c_str());
  std::string pending;
  int pendingWidth = 0;
  bool pendingIsIpa = false;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&bp))) {
    const bool combining = utf8IsCombiningMark(cp);
    const bool cpIsIpa = combining ? pendingIsIpa : isIpaCodepoint(cp);
    if (pending.empty()) pendingIsIpa = cpIsIpa;
    std::string cpStr;
    utf8AppendCodepoint(cp, cpStr);
    const int cpWidth = measure_(cpStr.c_str(), style, cpIsIpa);
    if (!pending.empty() && currentX_ + pendingWidth + cpWidth > maxWidth_) {
      appendMixed(pending.c_str(), style);
      flushLine();
      startLine(indentLevel, false);
      pending.clear();
      pendingWidth = 0;
      pendingIsIpa = cpIsIpa;
    }
    pending += cpStr;
    pendingWidth += cpWidth;
  }
  if (!pending.empty()) appendMixed(pending.c_str(), style);
}

void Wrapper::onSpan(const StyledSpan& span) {
  if (!span.text || span.text[0] == '\0') return;

  EpdFontFamily::Style style;
  if (span.bold && span.italic) {
    style = EpdFontFamily::BOLD_ITALIC;
  } else if (span.bold) {
    style = EpdFontFamily::BOLD;
  } else if (span.italic) {
    style = EpdFontFamily::ITALIC;
  } else {
    style = EpdFontFamily::REGULAR;
  }
  if (span.underline) style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::UNDERLINE);

  if (span.newlineBefore) {
    flushLine();
    startLine(span.indentLevel, span.isListItem);
  }

  const int spanWidth = getMixedWidth(span.text, style);
  if (!pendingInterSpanSpace_ && !hasAsciiSpace(span.text) && currentX_ + spanWidth <= maxWidth_) {
    // Fast path: a single no-space token fits on the current line.
    appendMixed(span.text, style);
    pendingInterSpanSpace_ = false;
  } else {
    // Word-wrap within the span.
    const char* p = span.text;
    while (*p) {
      bool hadSpace = false;
      while (*p == ' ') {
        hadSpace = true;
        ++p;
      }
      if (!*p) {
        pendingInterSpanSpace_ = hadSpace && !lineSegments_.empty();
        break;
      }

      const char* tokStart = p;
      while (*p && *p != ' ') ++p;
      const std::string tok(tokStart, p - tokStart);

      bool lineIsEmpty = lineSegments_.empty();
      bool useSpace = !lineIsEmpty && (hadSpace || pendingInterSpanSpace_);
      pendingInterSpanSpace_ = false;
      const int tokWidth = getMixedWidth(tok.c_str(), style);
      const int spaceWidth = useSpace ? measure_(" ", style, false) : 0;

      if (currentX_ + spaceWidth + tokWidth > maxWidth_ && !lineIsEmpty) {
        flushLine();
        startLine(span.indentLevel, false);
        useSpace = false;
      }

      if (currentX_ + (useSpace ? spaceWidth : 0) + tokWidth > maxWidth_) {
        breakToken(tok, style, span.indentLevel);
      } else {
        if (useSpace) appendToLine(" ", style, false, spaceWidth);
        appendMixed(tok.c_str(), style);
      }
    }
  }
}

void Wrapper::lineBreak(const uint8_t indent, const bool listItem) {
  flushLine();
  startLine(indent, listItem);
}

void Wrapper::finish() { flushLine(); }

// --------------------------------------------------------------------------
// Free-function drivers over Wrapper
// --------------------------------------------------------------------------

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink) {
  Wrapper wrapper(metrics, measure, sink);
  for (const auto& span : spans) wrapper.onSpan(span);
  wrapper.finish();
}

void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out) {
  out.clear();
  out.reserve(32);
  const LineSink sink{&out, [](void* ctx, const LayoutLineView& line) {
                        auto& lines = *static_cast<std::vector<LayoutLine>*>(ctx);
                        LayoutLine owned;
                        owned.indentLevel = line.indentLevel;
                        owned.isListItem = line.isListItem;
                        owned.segments.reserve(line.segmentCount);
                        for (uint16_t i = 0; i < line.segmentCount; ++i) {
                          const auto& segment = line.segments[i];
                          owned.segments.push_back({std::string(line.textPool + segment.offset, segment.length),
                                                    segment.style, segment.isIpa});
                        }
                        lines.push_back(std::move(owned));
                      }};
  wrapSpans(spans, metrics, measure, sink);
}

}  // namespace DictLayout
