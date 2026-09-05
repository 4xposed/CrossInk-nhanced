#pragma once

#include <EpdFontFamily.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

class GfxRenderer {
 public:
  struct PrewarmBatch {
    std::vector<uint32_t> codepoints;
    uint8_t styleMask = 0;
  };

  enum class Orientation : uint8_t {
    Portrait,
    LandscapeClockwise,
    PortraitInverted,
    LandscapeCounterClockwise,
  };

  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    ++measurementCalls;
    if (!text) return 0;
    const uint8_t styleIndex = static_cast<uint8_t>(style) & 0x03U;
    measurementStyleMask = static_cast<uint8_t>(measurementStyleMask | (1U << styleIndex));
    const char* cursor = text;
    while (*cursor != '\0') {
      const uint32_t codepoint = nextCodepoint(cursor);
      const auto& cache = advanceCache[styleIndex];
      if (std::find(cache.begin(), cache.end(), codepoint) == cache.end()) ++uncachedMeasurementCalls;
    }
    if (indentAdvance >= 0 && std::strcmp(text, "   ") == 0) return indentAdvance;
    const int bytes = static_cast<int>(std::strlen(text));
    return bytes + (style == EpdFontFamily::BOLD || style == EpdFontFamily::BOLD_ITALIC ? 1 : 0);
  }

  int getFontAscenderSize(int) const { return 8; }
  int getLineHeight(int) const { return 16; }

  void ensureSdCardFontReady(int, const uint32_t* codepoints, uint32_t count, bool includeSpace, bool includeHyphen,
                             uint8_t styleMask) const {
    ++prewarmCalls;
    prewarmStyleMask = static_cast<uint8_t>(prewarmStyleMask | styleMask);
    prewarmBatches.push_back({std::vector<uint32_t>(codepoints, codepoints + count), styleMask});
    std::vector<uint32_t> requested(codepoints, codepoints + count);
    if (includeSpace) requested.push_back(' ');
    if (includeHyphen) requested.push_back('-');
    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()), requested.end());
    for (uint8_t styleIndex = 0; styleIndex < 4; ++styleIndex) {
      if ((styleMask & (1U << styleIndex)) == 0) continue;
      auto& cache = advanceCache[styleIndex];
      if (cache.size() >= kAdvanceCacheCapacity &&
          std::any_of(requested.begin(), requested.end(),
                      [&](const uint32_t cp) { return std::find(cache.begin(), cache.end(), cp) == cache.end(); })) {
        cache.clear();
      }
      cache.insert(cache.end(), requested.begin(), requested.end());
      std::sort(cache.begin(), cache.end());
      cache.erase(std::unique(cache.begin(), cache.end()), cache.end());
      if (cache.size() > kAdvanceCacheCapacity) cache.resize(kAdvanceCacheCapacity);
    }
  }

  bool releaseSdCardFontForLowMemory(int, bool = false) const {
    ++releaseCalls;
    return releaseSucceeds;
  }

  void setOrientation(const Orientation value) { orientation_ = value; }
  Orientation getOrientation() const { return orientation_; }

  mutable uint32_t releaseCalls = 0;
  mutable uint32_t measurementCalls = 0;
  mutable uint32_t prewarmCalls = 0;
  mutable uint8_t prewarmStyleMask = 0;
  mutable uint8_t measurementStyleMask = 0;
  mutable uint32_t uncachedMeasurementCalls = 0;
  mutable std::vector<PrewarmBatch> prewarmBatches;
  mutable std::array<std::vector<uint32_t>, 4> advanceCache;
  int indentAdvance = -1;
  bool releaseSucceeds = true;

 private:
  static constexpr size_t kAdvanceCacheCapacity = 256;

  static uint32_t nextCodepoint(const char*& cursor) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(cursor);
    uint32_t codepoint = 0;
    if (bytes[0] < 0x80) {
      codepoint = bytes[0];
      cursor += 1;
    } else if ((bytes[0] & 0xE0U) == 0xC0U) {
      codepoint = ((bytes[0] & 0x1FU) << 6U) | (bytes[1] & 0x3FU);
      cursor += 2;
    } else if ((bytes[0] & 0xF0U) == 0xE0U) {
      codepoint = ((bytes[0] & 0x0FU) << 12U) | ((bytes[1] & 0x3FU) << 6U) | (bytes[2] & 0x3FU);
      cursor += 3;
    } else {
      codepoint =
          ((bytes[0] & 0x07U) << 18U) | ((bytes[1] & 0x3FU) << 12U) | ((bytes[2] & 0x3FU) << 6U) | (bytes[3] & 0x3FU);
      cursor += 4;
    }
    return codepoint;
  }

  Orientation orientation_ = Orientation::Portrait;
};
