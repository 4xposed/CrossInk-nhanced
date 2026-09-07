#pragma once

#include <GfxRenderer.h>

#include <string>

#include "QrCodePolicy.h"
#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Renders a QR code with the given text payload within the specified bounding box.
enum class DrawResult { Drawn, InvalidPayload, InvalidBounds, OutOfMemory, EncodingFailed };
// Encoding must run on the render task: the pinned v40 codec's known C3 stack
// subtotal is 11552 bytes before other callers/callees (not a safety proof).
DrawResult drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const char* payload, size_t length,
                      uint8_t* workBuffer = nullptr, size_t workCapacity = 0);
DrawResult drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

}  // namespace QrUtils
