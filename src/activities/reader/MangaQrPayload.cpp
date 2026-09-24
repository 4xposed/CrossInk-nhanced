#include "MangaQrPayload.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace manga {
namespace {
// Strict UTF-8 validation before the cap; malformed suffixes still fail after
// truncation. Returning a width avoids creating any intermediate strings.
size_t utf8Width(std::string_view text) {
  const auto b = static_cast<uint8_t>(text[0]);
  if (b < 0x80) return b ? 1 : 0;
  const size_t n = b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
  if (!n || text.size() < n) return 0;
  uint32_t cp = b & (0x7f >> n);
  for (size_t i = 1; i < n; ++i) {
    const auto c = static_cast<uint8_t>(text[i]);
    if ((c & 0xc0) != 0x80) return 0;
    cp = (cp << 6) | (c & 63);
  }
  return (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
                 (cp >= 0xd800 && cp <= 0xdfff)
             ? 0
             : n;
}
}  // namespace
QrPayloadResult buildQrPayload(format::PageView page, const int scope, QrUtils::OwnedPayload& out) {
  out = {};
  if (scope < -1 || (scope >= 0 && scope >= page.panels.remaining)) {
    LOG_ERR("MANGA", "Invalid QR OCR scope");
    return QrPayloadResult::Malformed;
  }
  // One child-owned buffer, <=2954 bytes, exceeds the small stack budget and
  // cannot be static across parent/child lifetimes. Allocated only on QR request.
  auto bytes = makeUniqueNoThrow<char[]>(QrUtils::kMaxPayloadBytes + 1);
  if (!bytes) {
    LOG_ERR("MANGA", "Cannot allocate QR payload (2954 bytes)");
    return QrPayloadResult::OutOfMemory;
  }
  size_t length = 0;
  bool truncated = false, hadBlock = false;
  auto panels = page.panels;
  for (int index = 0; panels.remaining; ++index) {
    format::PanelView panel;
    if (panels.next(panel) != format::Error::None) {
      LOG_ERR("MANGA", "Malformed QR panel metadata");
      return QrPayloadResult::Malformed;
    }
    if (scope >= 0 && index != scope) continue;
    auto texts = panel.texts;
    while (texts.remaining) {
      format::TextView block;
      if (texts.next(block) != format::Error::None) {
        LOG_ERR("MANGA", "Malformed QR OCR metadata");
        return QrPayloadResult::Malformed;
      }
      if (block.text.empty()) continue;
      if (hadBlock && !truncated) {
        if (length < QrUtils::kMaxPayloadBytes)
          bytes[length++] = '\n';
        else
          truncated = true;
      }
      hadBlock = true;
      for (size_t offset = 0; offset < block.text.size();) {
        const size_t width = utf8Width(block.text.substr(offset));
        if (!width) {
          LOG_ERR("MANGA", "Malformed QR OCR UTF-8");
          return QrPayloadResult::Malformed;
        }
        if (!truncated) {
          if (width <= QrUtils::kMaxPayloadBytes - length) {
            char* const payload = bytes.get();
            std::memcpy(payload + length, block.text.data() + offset, width);
            length += width;
          } else
            truncated = true;
        }
        offset += width;
      }
    }
  }
  if (!length) return QrPayloadResult::Empty;
  bytes[length] = '\0';
  out.bytes = std::move(bytes);
  out.length = static_cast<uint16_t>(length);
  out.truncated = truncated;
  if (truncated) LOG_INF("MANGA", "QR OCR truncated to %u UTF-8 bytes", out.length);
  return QrPayloadResult::Ready;
}
}  // namespace manga
