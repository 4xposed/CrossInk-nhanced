#include "PageWordScanner.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstddef>
namespace {
constexpr uint16_t kFallbackCandidateCapacity = 256;
constexpr size_t kMaxStarDictBytes = 255;
constexpr uint8_t kMaxJapaneseCodepoints = 8;
constexpr size_t kMaxJapaneseBytes = kMaxJapaneseCodepoints * 4;
constexpr uint8_t kMinCommonReadingPriority = 200;

struct JapaneseWindow {
  char bytes[kMaxJapaneseBytes]{};
  std::array<uint8_t, kMaxJapaneseCodepoints + 1> byteEnds{};
  uint8_t byteCount = 0;
  uint8_t glyphCount = 0;
};

bool isStarDictDelimiter(const PageTextGlyph& glyph) {
  return glyph.pageWord == PageTextGlyph::kSyntheticPageWord || glyph.codepoint == 0x2013 || glyph.codepoint == 0x2014;
}

bool isCjk(const uint32_t codepoint) {
  return (codepoint >= 0x4E00 && codepoint <= 0x9FFF) || (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF);
}

bool isKatakana(const uint32_t codepoint) {
  return (codepoint >= 0x30A0 && codepoint <= 0x30FF) || codepoint == 0x30FC;
}

bool isDigit(const uint32_t codepoint) {
  return (codepoint >= '0' && codepoint <= '9') || (codepoint >= 0xFF10 && codepoint <= 0xFF19);
}

bool isCaseParticle(const uint32_t codepoint) {
  switch (codepoint) {
    case 0x306B:  // に
    case 0x306E:  // の
    case 0x3068:  // と
    case 0x304C:  // が
    case 0x306F:  // は
    case 0x3092:  // を
    case 0x3082:  // も
    case 0x3067:  // で
    case 0x3078:  // へ
    case 0x3084:  // や
    case 0x304B:  // か
      return true;
    default:
      return false;
  }
}

bool isTrailingParticle(const uint32_t codepoint) {
  return codepoint == 0x306E || codepoint == 0x306F || codepoint == 0x304C || codepoint == 0x3092 ||
         codepoint == 0x306B || codepoint == 0x3078 || codepoint == 0x3082 || codepoint == 0x3068;
}

bool isDisplayNoise(const uint32_t codepoint) {
  switch (codepoint) {
    case 0x306F:
    case 0x304C:
    case 0x306E:
    case 0x306B:
    case 0x3067:
    case 0x3092:
    case 0x3082:
    case 0x3068:
    case 0x304B:
    case 0x306A:
    case 0x3078:
    case 0x3088:
    case 0x306D:
    case 0x308F:
    case 0x3066:
    case 0x3060:
    case 0x305F:
    case 0x308B:
    case 0x3093:
    case 0x3044:
    case 0x304F:
    case 0x3057:
    case 0x3055:
    case 0x305B:
    case 0x3089:
    case 0x304D:
    case 0x3053:
    case 0x305D:
    case 0x3042:
    case 0x304A:
    case 0x307E:
    case 0x3059:
    case 0x308C:
    case 0x3079:
    case 0x305E:
    case 0x3081:
    case 0x3076:
    case 0x307F:
    case 0x3064:
    case 0x306C:
    case 0x3075:
    case 0x3080:
      return true;
    default:
      return false;
  }
}

bool isSuppressedSmallKana(const uint32_t codepoint) {
  switch (codepoint) {
    case 0x3041:
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:
    case 0x3083:
    case 0x3085:
    case 0x3087:
    case 0x308E:
    case 0x30A1:
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:
    case 0x30C3:
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:
    case 0x30EE:
      return true;
    default:
      return false;
  }
}

bool encodeUtf8(const uint32_t codepoint, char* output, size_t capacity, size_t& length) {
  size_t bytes = 0;
  if (codepoint <= 0x7F) {
    bytes = 1;
  } else if (codepoint <= 0x7FF) {
    bytes = 2;
  } else if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    bytes = 3;
  } else if (codepoint <= 0x10FFFF) {
    bytes = 4;
  } else {
    return false;
  }
  if (bytes > capacity - length) return false;
  if (bytes == 1) {
    output[length++] = static_cast<char>(codepoint);
  } else if (bytes == 2) {
    output[length++] = static_cast<char>(0xC0 | (codepoint >> 6));
    output[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (bytes == 3) {
    output[length++] = static_cast<char>(0xE0 | (codepoint >> 12));
    output[length++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    output[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    output[length++] = static_cast<char>(0xF0 | (codepoint >> 18));
    output[length++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    output[length++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    output[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return true;
}

DictionaryStatus buildJapaneseWindow(const PageTextSourceView source, const uint16_t start, JapaneseWindow& out) {
  out = {};
  if (start >= source.glyphCount) return DictionaryStatus::NotFound;
  const uint16_t paragraph = source.glyphs[start].paragraph;
  for (uint16_t index = start; index < source.glyphCount && out.glyphCount < kMaxJapaneseCodepoints; ++index) {
    if (source.glyphs[index].paragraph != paragraph) break;
    size_t byteCount = out.byteCount;
    if (!encodeUtf8(source.glyphs[index].codepoint, out.bytes, sizeof(out.bytes), byteCount)) {
      return DictionaryStatus::ReadError;
    }
    out.byteCount = static_cast<uint8_t>(byteCount);
    out.byteEnds[++out.glyphCount] = out.byteCount;
  }
  return out.glyphCount == 0 ? DictionaryStatus::NotFound : DictionaryStatus::Found;
}

DictionaryStatus validateProbe(const DictionaryStatus status, const DictionaryProbeResult& result,
                               const JapaneseWindow& window, uint8_t& matchedGlyphs) {
  matchedGlyphs = 0;
  if (status != DictionaryStatus::Found) return status;
  if (result.status != DictionaryStatus::Found) return DictionaryStatus::ReadError;
  if (result.matchedBytes == 0 || result.matchedBytes > window.byteCount || result.matchedBytes > UINT8_MAX) {
    return DictionaryStatus::ReadError;
  }
  for (uint8_t count = 1; count <= window.glyphCount; ++count) {
    if (window.byteEnds[count] == result.matchedBytes) {
      matchedGlyphs = count;
      return DictionaryStatus::Found;
    }
  }
  return DictionaryStatus::ReadError;
}

bool matchesSequence(const PageTextSourceView source, const uint16_t start, const uint32_t* expected,
                     const uint8_t count) {
  if (start > source.glyphCount || count > source.glyphCount - start) return false;
  const uint16_t paragraph = source.glyphs[start].paragraph;
  for (uint8_t index = 0; index < count; ++index) {
    if (source.glyphs[start + index].paragraph != paragraph ||
        source.glyphs[start + index].codepoint != expected[index]) {
      return false;
    }
  }
  return true;
}

bool isExactNoise(const PageTextSourceView source, const uint16_t start, const uint8_t matchedGlyphs) {
  static constexpr uint32_t kCha[] = {0x3061, 0x3083};
  static constexpr uint32_t kJa[] = {0x3058, 0x3083};
  static constexpr uint32_t kChau[] = {0x3061, 0x3083, 0x3046};
  static constexpr uint32_t kTte[] = {0x3063, 0x3066};
  static constexpr uint32_t kSouni[] = {0x305D, 0x3046, 0x306B};
  static constexpr uint32_t kSouna[] = {0x305D, 0x3046, 0x306A};
  static constexpr uint32_t kSouda[] = {0x305D, 0x3046, 0x3060};
  return (matchedGlyphs == 2 && (matchesSequence(source, start, kCha, 2) || matchesSequence(source, start, kJa, 2) ||
                                 matchesSequence(source, start, kTte, 2))) ||
         (matchedGlyphs == 3 &&
          (matchesSequence(source, start, kChau, 3) || matchesSequence(source, start, kSouni, 3) ||
           matchesSequence(source, start, kSouna, 3) || matchesSequence(source, start, kSouda, 3)));
}

bool isConjugationNoise(const PageTextSourceView source, const uint16_t start) {
  static constexpr uint32_t kMashita[] = {0x307E, 0x3057, 0x305F};
  static constexpr uint32_t kMasen[] = {0x307E, 0x305B, 0x3093};
  static constexpr uint32_t kDeshita[] = {0x3067, 0x3057, 0x305F};
  static constexpr uint32_t kDesu[] = {0x3067, 0x3059};
  static constexpr uint32_t kMasu[] = {0x307E, 0x3059};
  return matchesSequence(source, start, kMashita, 3) || matchesSequence(source, start, kMasen, 3) ||
         matchesSequence(source, start, kDeshita, 3) || matchesSequence(source, start, kDesu, 2) ||
         matchesSequence(source, start, kMasu, 2);
}

bool passesDisplayFilter(const PageTextSourceView source, const uint16_t start, const uint8_t matchedGlyphs) {
  bool allNoise = matchedGlyphs <= 2;
  for (uint8_t index = 0; allNoise && index < matchedGlyphs; ++index) {
    if (!isDisplayNoise(source.glyphs[start + index].codepoint)) allNoise = false;
  }
  return !allNoise && !isExactNoise(source, start, matchedGlyphs) && !isConjugationNoise(source, start);
}

uint8_t katakanaRunBeforeHonorific(const PageTextSourceView source, const uint16_t start,
                                   const JapaneseWindow& window) {
  uint8_t run = 0;
  while (run < window.glyphCount) {
    const uint32_t codepoint = source.glyphs[start + run].codepoint;
    if (!isKatakana(codepoint) || codepoint == 0x30FB) break;
    ++run;
  }
  if (run < 2 || run >= window.glyphCount) return 0;
  const auto at = [&](const uint8_t offset) -> uint32_t {
    return offset < window.glyphCount ? source.glyphs[start + offset].codepoint : 0;
  };
  const bool honorific = (at(run) == 0x3055 && (at(run + 1) == 0x3093 || at(run + 1) == 0x307E)) ||
                         (at(run) == 0x304F && at(run + 1) == 0x3093) ||
                         (at(run) == 0x3061 && at(run + 1) == 0x3083 && at(run + 2) == 0x3093) || at(run) == 0x69D8 ||
                         at(run) == 0x6C0F;
  return honorific ? run : 0;
}
}  // namespace

DictionaryStatus PageWordScanner::allocateCandidates(const bool preferFullCapacity) {
  candidates_.reset();
  candidateCapacity_ = 0;
  const uint16_t fullCapacity = source_.glyphCount;
  if (fullCapacity == 0) return DictionaryStatus::Found;

  if (preferFullCapacity) {
    candidates_ = makeUniqueNoThrow<PageWordCandidate[]>(fullCapacity);
    if (candidates_) {
      candidateCapacity_ = fullCapacity;
      return DictionaryStatus::Found;
    }
    LOG_ERR("WLS", "OOM: %u byte full page candidate array",
            static_cast<unsigned>(static_cast<size_t>(fullCapacity) * sizeof(PageWordCandidate)));
  }

  const uint16_t fallbackCapacity = std::min<uint16_t>(fullCapacity, kFallbackCandidateCapacity);
  candidates_ = makeUniqueNoThrow<PageWordCandidate[]>(fallbackCapacity);
  if (!candidates_) {
    LOG_ERR("WLS", "OOM: %u byte fallback page candidate array",
            static_cast<unsigned>(static_cast<size_t>(fallbackCapacity) * sizeof(PageWordCandidate)));
    return DictionaryStatus::OutOfMemory;
  }
  candidateCapacity_ = fallbackCapacity;
  truncated_ = true;
  return DictionaryStatus::Found;
}

DictionaryStatus PageWordScanner::begin(const PageTextSourceView source, const DictionaryBackendKind backend,
                                        const DictionaryProbeFn probe,
                                        const PageWordScannerMemoryRecoveryFn memoryRecovery) {
  clear();
  if ((source.glyphCount != 0 && !source.glyphs) || !probe.call) return DictionaryStatus::Unavailable;
  source_ = source;
  backend_ = backend;
  probe_ = probe;
  const DictionaryStatus status = allocateCandidates(true);
  if (status != DictionaryStatus::Found) {
    clear();
    return status;
  }
  initialized_ = true;
  done_ = source.glyphCount == 0;
  if (truncated_ && memoryRecovery.release) {
    memoryRecovery.release(memoryRecovery.context);
    const DictionaryStatus retryStatus = restart();
    if (retryStatus != DictionaryStatus::Found) {
      clear();
      return retryStatus;
    }
  }
  return DictionaryStatus::Found;
}

DictionaryStatus PageWordScanner::scanStarDict() {
  const auto& first = source_.glyphs[scanPos_];
  if (isStarDictDelimiter(first)) {
    ++scanPos_;
    done_ = scanPos_ >= source_.glyphCount;
    return DictionaryStatus::NotFound;
  }

  const uint16_t firstGlyph = scanPos_;
  const uint16_t pageWord = first.pageWord;
  uint16_t end = firstGlyph;
  char token[kMaxStarDictBytes + 1]{};
  size_t tokenBytes = 0;
  bool encodable = true;
  while (end < source_.glyphCount) {
    const auto& glyph = source_.glyphs[end];
    if (isStarDictDelimiter(glyph) || glyph.pageWord != pageWord) break;
    if (!encodeUtf8(glyph.codepoint, token, kMaxStarDictBytes, tokenBytes)) encodable = false;
    ++end;
  }
  scanPos_ = end;
  done_ = scanPos_ >= source_.glyphCount;
  if (!encodable || end - firstGlyph > UINT8_MAX) return DictionaryStatus::NotFound;
  token[tokenBytes] = '\0';
  if (!utf8ContainsLookupCharacter(token)) return DictionaryStatus::NotFound;

  DictionaryProbeResult result;
  const DictionaryStatus status =
      probe_.call(probe_.context, {{token, tokenBytes}, 0, DictionaryLookupMode::Token}, result);
  if (status != DictionaryStatus::Found && status != DictionaryStatus::NotFound) return status;
  if (result.status != status) return DictionaryStatus::ReadError;
  size_t matchedBytes = tokenBytes;
  if (status == DictionaryStatus::Found) {
    if (result.matchedBytes != tokenBytes || result.matchedBytes == 0 || result.matchedBytes > UINT8_MAX) {
      return DictionaryStatus::ReadError;
    }
    matchedBytes = result.matchedBytes;
  }
  if (candidateCount_ >= candidateCapacity_) {
    LOG_ERR("WLS", "Page candidate capacity exhausted at %u entries", static_cast<unsigned>(candidateCount_));
    truncated_ = true;
    done_ = true;
    return DictionaryStatus::OutOfMemory;
  }
  candidates_[candidateCount_++] = {firstGlyph, static_cast<uint8_t>(end - firstGlyph),
                                    static_cast<uint8_t>(matchedBytes), pageWord, pageWord};
  return DictionaryStatus::Found;
}

DictionaryStatus PageWordScanner::scanJapanese() {
  const uint16_t firstGlyph = scanPos_++;
  done_ = scanPos_ >= source_.glyphCount;
  if (firstGlyph < skipUntil_) return DictionaryStatus::NotFound;

  const uint16_t paragraph = source_.glyphs[firstGlyph].paragraph;
  uint16_t scanStart = firstGlyph;
  while (scanStart < source_.glyphCount && source_.glyphs[scanStart].paragraph == paragraph &&
         isDigit(source_.glyphs[scanStart].codepoint)) {
    ++scanStart;
  }
  const uint16_t digitGlyphs = scanStart - firstGlyph;
  if (scanStart >= source_.glyphCount || source_.glyphs[scanStart].paragraph != paragraph) {
    return DictionaryStatus::NotFound;
  }

  bool sokuonTeStart = false;
  const uint32_t firstCodepoint = source_.glyphs[scanStart].codepoint;
  if (firstCodepoint == 0x3063) {
    const bool teNext = scanStart + 1 < source_.glyphCount && source_.glyphs[scanStart + 1].paragraph == paragraph &&
                        source_.glyphs[scanStart + 1].codepoint == 0x3066;
    if (!teNext) return DictionaryStatus::NotFound;
    sokuonTeStart = true;
  } else if (isSuppressedSmallKana(firstCodepoint)) {
    return DictionaryStatus::NotFound;
  }

  JapaneseWindow window;
  DictionaryStatus status = buildJapaneseWindow(source_, scanStart, window);
  if (status != DictionaryStatus::Found) return status;
  DictionaryProbeResult result;
  status =
      probe_.call(probe_.context, {{window.bytes, window.byteCount}, 0, DictionaryLookupMode::LongestAtOffset}, result);
  uint8_t matchedGlyphs = 0;
  status = validateProbe(status, result, window, matchedGlyphs);
  if (status != DictionaryStatus::Found) return status;
  if (sokuonTeStart && result.transformed) return DictionaryStatus::NotFound;

  if (matchedGlyphs >= 2 && isTrailingParticle(source_.glyphs[scanStart + matchedGlyphs - 1].codepoint) &&
      isCjk(source_.glyphs[scanStart + matchedGlyphs - 2].codepoint)) {
    JapaneseWindow stemWindow = window;
    stemWindow.glyphCount = static_cast<uint8_t>(matchedGlyphs - 1);
    stemWindow.byteCount = stemWindow.byteEnds[stemWindow.glyphCount];
    DictionaryProbeResult stemResult;
    status =
        probe_.call(probe_.context,
                    {{stemWindow.bytes, stemWindow.byteCount}, 0, DictionaryLookupMode::LongestAtOffset}, stemResult);
    uint8_t stemGlyphs = 0;
    status = validateProbe(status, stemResult, stemWindow, stemGlyphs);
    if (status == DictionaryStatus::Found && stemGlyphs == stemWindow.glyphCount &&
        stemResult.matchedBytes == stemWindow.byteCount) {
      result = stemResult;
      matchedGlyphs = stemGlyphs;
    } else if (status != DictionaryStatus::NotFound && status != DictionaryStatus::Found) {
      return status;
    }
  }

  const uint8_t filterGlyphs = matchedGlyphs;
  bool allKana = !isCjk(firstCodepoint) && !isKatakana(firstCodepoint);
  for (uint8_t index = 0; allKana && index < filterGlyphs; ++index) {
    const uint32_t codepoint = source_.glyphs[scanStart + index].codepoint;
    if (isCjk(codepoint) || isKatakana(codepoint)) allKana = false;
  }
  if (allKana && !result.transformed && filterGlyphs >= 2 &&
      (result.posFlags & DictionaryProbeResult::kReadingRecord) != 0 && result.priority < kMinCommonReadingPriority) {
    return DictionaryStatus::NotFound;
  }

  if (filterGlyphs >= 2 && isCaseParticle(firstCodepoint) && scanStart + 1 < source_.glyphCount &&
      source_.glyphs[scanStart + 1].paragraph == paragraph) {
    JapaneseWindow nextWindow;
    status = buildJapaneseWindow(source_, static_cast<uint16_t>(scanStart + 1), nextWindow);
    if (status != DictionaryStatus::Found) return status;
    DictionaryProbeResult nextResult;
    status =
        probe_.call(probe_.context,
                    {{nextWindow.bytes, nextWindow.byteCount}, 0, DictionaryLookupMode::LongestAtOffset}, nextResult);
    uint8_t nextGlyphs = 0;
    status = validateProbe(status, nextResult, nextWindow, nextGlyphs);
    if (status == DictionaryStatus::Found && nextGlyphs >= filterGlyphs) return DictionaryStatus::NotFound;
    if (status != DictionaryStatus::NotFound && status != DictionaryStatus::Found) return status;
  }

  if (isKatakana(firstCodepoint)) {
    const uint8_t nameRun = katakanaRunBeforeHonorific(source_, scanStart, window);
    if (nameRun > matchedGlyphs) matchedGlyphs = nameRun;
  }

  const uint16_t candidateEnd = static_cast<uint16_t>(scanStart + matchedGlyphs);
  if (matchedGlyphs > 1 || digitGlyphs > 0) {
    skipUntil_ = candidateEnd;
    if (candidateEnd < source_.glyphCount && source_.glyphs[candidateEnd].paragraph == paragraph) {
      const uint32_t next = source_.glyphs[candidateEnd].codepoint;
      if (next == 0x3055 && candidateEnd + 1 < source_.glyphCount) {
        const uint32_t after = source_.glyphs[candidateEnd + 1].codepoint;
        if (after == 0x3093) skipUntil_ = static_cast<uint16_t>(candidateEnd + 2);
        if (after == 0x307E && candidateEnd + 2 < source_.glyphCount &&
            source_.glyphs[candidateEnd + 2].paragraph == paragraph) {
          skipUntil_ = static_cast<uint16_t>(candidateEnd + 2);
        }
      } else if (next == 0x304F && candidateEnd + 1 < source_.glyphCount &&
                 source_.glyphs[candidateEnd + 1].codepoint == 0x3093) {
        skipUntil_ = static_cast<uint16_t>(candidateEnd + 2);
      } else if (next == 0x3061 && candidateEnd + 2 < source_.glyphCount &&
                 source_.glyphs[candidateEnd + 1].codepoint == 0x3083 &&
                 source_.glyphs[candidateEnd + 2].codepoint == 0x3093) {
        skipUntil_ = static_cast<uint16_t>(candidateEnd + 3);
      } else if (next == 0x6C0F || next == 0x69D8) {
        skipUntil_ = static_cast<uint16_t>(candidateEnd + 1);
      } else if ((next == 0x58EB || next == 0x5E2B || next == 0x54E1) &&
                 isCjk(source_.glyphs[scanStart + matchedGlyphs - 1].codepoint)) {
        skipUntil_ = static_cast<uint16_t>(candidateEnd + 1);
      }
    }
  }

  // Matcha advances skipUntil before filtering so progressive results never
  // disappear and filtered match interiors never become later candidates.
  if (digitGlyphs == 0 && !passesDisplayFilter(source_, scanStart, filterGlyphs)) return DictionaryStatus::NotFound;

  const uint16_t candidateGlyphs = candidateEnd - firstGlyph;
  if (candidateGlyphs == 0 || candidateGlyphs > UINT8_MAX || result.matchedBytes > UINT8_MAX) {
    return DictionaryStatus::ReadError;
  }
  uint16_t firstPageWord = PageTextGlyph::kSyntheticPageWord;
  uint16_t lastPageWord = PageTextGlyph::kSyntheticPageWord;
  for (uint16_t index = firstGlyph; index < candidateEnd; ++index) {
    const uint16_t pageWord = source_.glyphs[index].pageWord;
    if (pageWord == PageTextGlyph::kSyntheticPageWord) continue;
    if (firstPageWord == PageTextGlyph::kSyntheticPageWord) firstPageWord = pageWord;
    lastPageWord = pageWord;
  }
  if (firstPageWord == PageTextGlyph::kSyntheticPageWord) return DictionaryStatus::NotFound;
  if (candidateCount_ >= candidateCapacity_) {
    LOG_ERR("WLS", "Page candidate capacity exhausted at %u entries", static_cast<unsigned>(candidateCount_));
    truncated_ = true;
    done_ = true;
    return DictionaryStatus::OutOfMemory;
  }
  candidates_[candidateCount_++] = {firstGlyph, static_cast<uint8_t>(candidateGlyphs),
                                    static_cast<uint8_t>(result.matchedBytes), firstPageWord, lastPageWord};
  return DictionaryStatus::Found;
}

DictionaryStatus PageWordScanner::stepOne() {
  if (!initialized_) return DictionaryStatus::Unavailable;
  if (terminalStatus_ != DictionaryStatus::Found) return terminalStatus_;
  if (done_) return DictionaryStatus::NotFound;
  if (scanPos_ >= source_.glyphCount) {
    done_ = true;
    return DictionaryStatus::NotFound;
  }
  const DictionaryStatus status = backend_ == DictionaryBackendKind::StarDict ? scanStarDict() : scanJapanese();
  if (status != DictionaryStatus::Found && status != DictionaryStatus::NotFound) {
    terminalStatus_ = status;
    done_ = true;
  } else if (done_ && truncated_) {
    terminalStatus_ = DictionaryStatus::OutOfMemory;
    return terminalStatus_;
  }
  return status;
}

const PageWordCandidate* PageWordScanner::candidate(const uint16_t index) const {
  return candidates_ && index < candidateCount_ ? &candidates_[index] : nullptr;
}

DictionaryStatus PageWordScanner::restart() {
  if (!initialized_) return DictionaryStatus::Unavailable;
  candidateCount_ = 0;
  scanPos_ = 0;
  skipUntil_ = 0;
  done_ = source_.glyphCount == 0;
  truncated_ = false;
  terminalStatus_ = allocateCandidates(true);
  if (terminalStatus_ != DictionaryStatus::Found) {
    truncated_ = true;
    done_ = true;
  }
  return terminalStatus_;
}

void PageWordScanner::clear() {
  candidates_.reset();
  source_ = {};
  probe_ = {};
  terminalStatus_ = DictionaryStatus::Found;
  candidateCapacity_ = 0;
  candidateCount_ = 0;
  scanPos_ = 0;
  skipUntil_ = 0;
  initialized_ = false;
  done_ = false;
  truncated_ = false;
}
