#include "WordLookup.h"

#include <array>
#include <cstring>

namespace {

bool decodeUtf8(std::string_view text, size_t offset, uint32_t& codepoint, size_t& length) {
  if (offset >= text.size()) return false;
  const auto first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) {
    codepoint = first;
    length = 1;
    return true;
  }

  uint32_t minimum = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    codepoint = first & 0x1F;
    length = 2;
    minimum = 0x80;
  } else if (first >= 0xE0 && first <= 0xEF) {
    codepoint = first & 0x0F;
    length = 3;
    minimum = 0x800;
  } else if (first >= 0xF0 && first <= 0xF4) {
    codepoint = first & 0x07;
    length = 4;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (offset + length > text.size()) return false;
  for (size_t index = 1; index < length; ++index) {
    const auto next = static_cast<uint8_t>(text[offset + index]);
    if ((next & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (next & 0x3F);
  }
  return codepoint >= minimum && codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
}

bool validUtf8(std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    uint32_t codepoint = 0;
    size_t length = 0;
    if (!decodeUtf8(text, offset, codepoint, length)) return false;
    offset += length;
  }
  return true;
}

bool isHiragana(uint32_t codepoint) { return codepoint >= 0x3040 && codepoint <= 0x309F; }

bool isNameCharacter(uint32_t codepoint) {
  return (codepoint >= 0x4E00 && codepoint <= 0x9FFF) || (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF) || (codepoint >= 0x30A0 && codepoint <= 0x30FF) ||
         (codepoint >= 0xFF66 && codepoint <= 0xFF9D);
}

bool hasNameCharacter(std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    uint32_t codepoint = 0;
    size_t length = 0;
    if (!decodeUtf8(text, offset, codepoint, length)) return false;
    if (isNameCharacter(codepoint)) return true;
    offset += length;
  }
  return false;
}

uint8_t posMask(WordCondition condition) {
  switch (condition) {
    case WordCondition::V1:
      return DictIndexRecord::POS_V1;
    case WordCondition::V5:
      return DictIndexRecord::POS_V5;
    case WordCondition::VS:
      return DictIndexRecord::POS_VS;
    case WordCondition::VK:
      return DictIndexRecord::POS_VK;
    case WordCondition::ADJ_I:
      return DictIndexRecord::POS_ADJ_I;
    case WordCondition::DICT:
      return 0;
  }
  return 0;
}

}  // namespace

JapaneseDictStatus WordLookup::find(std::string_view text, size_t byteOffset, Match& match) {
  match = Match{};
  if (byteOffset >= text.size() || !validUtf8(text) ||
      (byteOffset > 0 && (static_cast<uint8_t>(text[byteOffset]) & 0xC0) == 0x80)) {
    return JapaneseDictStatus::NotFound;
  }

  std::array<size_t, MAX_WINDOW_CHARS + 1> ends{};
  ends[0] = byteOffset;
  uint8_t characterCount = 0;
  size_t offset = byteOffset;
  while (offset < text.size() && characterCount < MAX_WINDOW_CHARS) {
    uint32_t codepoint = 0;
    size_t length = 0;
    if (!decodeUtf8(text, offset, codepoint, length)) return JapaneseDictStatus::NotFound;
    offset += length;
    ends[++characterCount] = offset;
  }

  for (uint8_t windowCharacters = characterCount; windowCharacters > 0; --windowCharacters) {
    const size_t windowLength = ends[windowCharacters] - byteOffset;
    const std::string_view window = text.substr(byteOffset, windowLength);
    uint8_t dictMask = DictIndex::DICT_JMDICT | DictIndex::DICT_GRAMMAR;
    if (hasNameCharacter(window)) dictMask |= DictIndex::DICT_NAMES;

    DictProbe probe;
    JapaneseDictStatus status = index_.probeExact(window, probe, dictMask);
    if (status == JapaneseDictStatus::Found) {
      std::memcpy(match.headword, window.data(), window.size());
      match.headwordLength = static_cast<uint8_t>(window.size());
      match.dictMask = probe.sourceDict;
      match.matchLength = windowLength;
      match.priority = probe.priority;
      match.posFlags = probe.posFlags;
      return JapaneseDictStatus::Found;
    }
    if (status != JapaneseDictStatus::NotFound) return status;

    uint32_t lastCodepoint = 0;
    size_t lastLength = 0;
    if (!decodeUtf8(text, ends[windowCharacters - 1], lastCodepoint, lastLength) || !isHiragana(lastCodepoint)) {
      continue;
    }

    Deinflector::deinflect(window, candidates_);
    for (uint8_t candidateIndex = 1; candidateIndex < candidates_.count; ++candidateIndex) {
      const DeinflectionCandidate& candidate = candidates_.candidates[candidateIndex];
      const std::string_view headword(candidate.text, candidate.byteLength);
      const uint8_t requiredPos = posMask(candidate.condition);
      status = index_.probeExact(headword, probe, DictIndex::DICT_JMDICT, requiredPos);
      if (status == JapaneseDictStatus::Found) {
        std::memcpy(match.headword, candidate.text, candidate.byteLength);
        match.headwordLength = candidate.byteLength;
        match.dictMask = DictIndex::DICT_JMDICT;
        match.posMask = requiredPos;
        match.matchLength = windowLength;
        match.deinflected = true;
        match.priority = probe.priority;
        match.posFlags = probe.posFlags;
        return JapaneseDictStatus::Found;
      }
      if (status != JapaneseDictStatus::NotFound) return status;
    }
  }
  return JapaneseDictStatus::NotFound;
}

JapaneseDictStatus WordLookup::probe(std::string_view text, size_t byteOffset, WordLookupProbe& out) {
  out = WordLookupProbe{};
  Match match;
  const JapaneseDictStatus status = find(text, byteOffset, match);
  if (status != JapaneseDictStatus::Found) return status;
  out.matchLength = match.matchLength;
  out.deinflected = match.deinflected;
  out.sourceDict = match.dictMask;
  out.priority = match.priority;
  out.posFlags = match.posFlags;
  uint32_t first = 0;
  size_t firstBytes = 0;
  if (decodeUtf8(text, byteOffset, first, firstBytes) &&
      (isHiragana(first) || (first >= 0x30A0 && first <= 0x30FF) || (first >= 0xFF66 && first <= 0xFF9D))) {
    const auto markerStatus =
        index_.checkUsuallyKana({match.headword, match.headwordLength}, out.usuallyKana, match.dictMask, match.posMask);
    out.markerCheckFailed = markerStatus != JapaneseDictStatus::Found;
  }
  return JapaneseDictStatus::Found;
}

JapaneseDictStatus WordLookup::lookup(std::string_view text, size_t byteOffset, WordLookupResult& out) {
  out.entry.reset();
  out.matchLength = 0;
  out.deinflected = false;
  Match match;
  JapaneseDictStatus status = find(text, byteOffset, match);
  if (status != JapaneseDictStatus::Found) return status;
  status = index_.lookupExact(std::string_view(match.headword, match.headwordLength), out.entry, match.dictMask,
                              match.posMask);
  if (status != JapaneseDictStatus::Found) return status;
  out.matchLength = match.matchLength;
  out.deinflected = match.deinflected;
  return JapaneseDictStatus::Found;
}
