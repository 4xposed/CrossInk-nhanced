#include "QrUtils.h"

#include <Memory.h>
#include <Utf8.h>
#include <qrcode.h>

#include <algorithm>

#include "Logging.h"

QrUtils::DrawResult QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const char* payload,
                                        const size_t length, uint8_t* workBuffer, const size_t workCapacity) {
  const uint8_t version = versionForBytes(length);
  if (!payload || !version) {
    LOG_ERR("QR", "Invalid QR payload size %u", static_cast<unsigned>(length));
    return DrawResult::InvalidPayload;
  }
  const int side = 17 + 4 * version;
  // Keep the four-module quiet zone inside bounds; never draw an oversized QR.
  const int pixels = std::min(bounds.width, bounds.height) / (side + 8);
  if (bounds.x < 0 || bounds.y < 0 || pixels < 1 || bounds.width > renderer.getScreenWidth() - bounds.x ||
      bounds.height > renderer.getScreenHeight() - bounds.y) {
    LOG_ERR("QR", "QR does not fit available bounds");
    return DrawResult::InvalidBounds;
  }
  const size_t needed = gridBytesForPayload(length);
  std::unique_ptr<uint8_t[]> owned;
  if (!workBuffer) {
    // Legacy network callers have no persistent QR owner. <=3917-byte scratch
    // exceeds the small stack budget; QR activities supply reusable owned bytes.
    owned = makeUniqueNoThrow<uint8_t[]>(needed);
    workBuffer = owned.get();
    if (!workBuffer) {
      LOG_ERR("QR", "Cannot allocate %u module bytes", static_cast<unsigned>(needed));
      return DrawResult::OutOfMemory;
    }
  } else if (workCapacity < needed) {
    LOG_ERR("QR", "QR module buffer too small");
    return DrawResult::OutOfMemory;
  }
  QRCode code{};
  // The pinned encoder only reads input despite its mutable pointer signature.
  // Capacity was checked BEFORE encoding: its return code cannot prevent overflow.
  if (qrcode_initBytes(&code, workBuffer, version, ECC_LOW, reinterpret_cast<uint8_t*>(const_cast<char*>(payload)),
                       static_cast<uint16_t>(length)) != 0) {
    LOG_ERR("QR", "QR encoding failed");
    return DrawResult::EncodingFailed;
  }
  renderer.fillRect(bounds.x, bounds.y, bounds.width, bounds.height, false);
  const int left = bounds.x + (bounds.width - side * pixels) / 2;
  const int top = bounds.y + (bounds.height - side * pixels) / 2;
  for (uint8_t y = 0; y < code.size; ++y)
    for (uint8_t x = 0; x < code.size; ++x)
      if (qrcode_getModule(&code, x, y)) renderer.fillRect(left + x * pixels, top + y * pixels, pixels, pixels, true);
  return DrawResult::Drawn;
}

QrUtils::DrawResult QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& text) {
  // Preserve legacy callers' UTF-8 boundary truncation without a second string.
  const size_t length = text.size() > kMaxPayloadBytes
                            ? static_cast<size_t>(utf8SafeTruncateBuffer(text.c_str(), kMaxPayloadBytes))
                            : text.size();
  return drawQrCode(renderer, bounds, text.data(), length);
}
