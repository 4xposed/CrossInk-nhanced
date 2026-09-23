#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {

char* appendUtf8Codepoint(char* output, const uint32_t codepoint) {
  if (codepoint < 0x80) {
    *output++ = static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return output;
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseSdFontCaches() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseForLowMemory(false);
  }
}

bool FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask,
                                    const PreparationPolicy policy) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed =
        it->second->prewarm(utf8Text, styleMask, /*metadataOnly=*/false, policy != PreparationPolicy::DictionaryLean);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return !it->second->lastPrewarmFailed();
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return false;

  // Reverse iteration is harmless now; the decompressor keeps one retained page slot per style.
  for (int8_t i = 3; i >= 0; i--) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
  return true;
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

uint8_t FontCacheManager::resolveScanStyle(int fontId, EpdFontFamily::Style style) const {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end()) return sdFont->second->resolveStyle(baseStyle);

  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return baseStyle;

  const EpdFontData* resolvedData = font->second.getData(static_cast<EpdFontFamily::Style>(baseStyle));
  for (uint8_t candidate = 0; candidate < 4; candidate++) {
    if (font->second.getData(static_cast<EpdFontFamily::Style>(candidate)) == resolvedData) return candidate;
  }
  return baseStyle;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  const auto family = fontMap_.find(fontId);
  const uint8_t primaryStyle = resolveScanStyle(fontId, style);
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;
    if (utf8IsVariationSelector(codepoint)) continue;
    if ((style & EpdFontFamily::SMALL_CAPS) != 0 && codepoint >= 'a' && codepoint <= 'z') {
      codepoint -= 'a' - 'A';
    }

    // The rendering face can differ from the primary face for Japanese,
    // symbols, or missing styled glyphs. Warm its compressed bitmap once per
    // page instead of decompressing groups repeatedly for each drawn glyph.
    const EpdFontData* fallbackData = nullptr;
    if (family != fontMap_.end()) {
      const auto* owner = family->second.getCoverageData(codepoint, style);
      if (owner && owner->groups && owner != family->second.getData(style)) fallbackData = owner;
    }
    uint8_t fontSlot = scanFontCount_;
    for (uint8_t i = 0; i < scanFontCount_; ++i) {
      if (scanFontData_[i] == fallbackData && (fallbackData || scanFontIds_[i] == fontId)) {
        fontSlot = i;
        break;
      }
    }
    if (fontSlot == scanFontCount_) {
      if (scanFontCount_ >= MAX_SCAN_FONTS) continue;
      scanFontIds_[fontSlot] = fontId;
      scanFontData_[fontSlot] = fallbackData;
      ++scanFontCount_;
    }
    const uint8_t resolvedStyle = fallbackData ? 0 : primaryStyle;
    const uint8_t group = fontSlot * 4 + resolvedStyle;
    const uint32_t packed = (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) |
                            (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT) | codepoint;
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == packed) {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager, const PreparationPolicy policy)
    : manager_(&manager), policy_(policy) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  manager_->scanOverflowWarned_ = false;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
}

bool FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanCodepointCount_ == 0) return true;

  std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_);

  uint16_t groupStarts[SCAN_GROUP_COUNT] = {};
  for (uint8_t group = 1; group < SCAN_GROUP_COUNT; group++) {
    groupStarts[group] = groupStarts[group - 1] + manager_->scanGroupCounts_[group - 1];
  }

  // Each packed entry provides four bytes, enough for one UTF-8 codepoint.
  // Encoding high groups first means a terminator can overwrite only a group
  // that has already been prewarmed; unread lower groups remain intact.
  bool ok = true;
  for (int group = SCAN_GROUP_COUNT - 1; group >= 0; group--) {
    const uint16_t groupCount = manager_->scanGroupCounts_[group];
    if (groupCount == 0) continue;

    const uint16_t groupStart = groupStarts[group];
    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStart);
    char* output = utf8Text;
    for (uint16_t i = 0; i < groupCount; i++) {
      const uint32_t codepoint = manager_->scanCodepoints_[groupStart + i] & SCAN_CODEPOINT_MASK;
      output = appendUtf8Codepoint(output, codepoint);
    }
    *output = '\0';

    const uint8_t fontSlot = static_cast<uint8_t>(group) / 4;
    const uint8_t style = static_cast<uint8_t>(group) & 0x03;
    if (const auto* data = manager_->scanFontData_[fontSlot]) {
      if (!manager_->fontDecompressor_) {
        ok = false;
      } else {
        const int missed = manager_->fontDecompressor_->prewarmCache(data, utf8Text);
        // Match primary compressed-font preparation: allocation pressure can
        // fall back to on-demand decompression without dropping the page.
        if (missed > 0) LOG_DBG("FCM", "Fallback prewarm: %d glyph(s) remain on demand", missed);
      }
    } else if (!manager_->prewarmCache(manager_->scanFontIds_[fontSlot], utf8Text, 1 << style, policy_)) {
      ok = false;
    }
  }

  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
  return ok;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called
    if (auto* decompressor = manager_->getDecompressor()) {
      const auto& stats = decompressor->getStats();
      if (stats.getBitmapTimeUs >= 1000000) {
        LOG_INF("FCM", "Slow glyph rendering: bitmap=%luus calls=%lu hits=%lu misses=%lu decompress=%lums",
                static_cast<unsigned long>(stats.getBitmapTimeUs), static_cast<unsigned long>(stats.getBitmapCalls),
                static_cast<unsigned long>(stats.cacheHits), static_cast<unsigned long>(stats.cacheMisses),
                static_cast<unsigned long>(stats.decompressTimeMs));
      }
    }
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), policy_(other.policy_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope(const PreparationPolicy policy) {
  return PrewarmScope(*this, policy);
}
