#pragma once
#include <MangaFormat.h>

#include "util/QrCodePolicy.h"
namespace manga {
enum class QrPayloadResult { Ready, Empty, Malformed, OutOfMemory };
// Same immutable page and -1/all versus selected-panel scope as lookup. Copies
// original OCR block bytes in stored order; never translation or glyph-limited UI text.
QrPayloadResult buildQrPayload(format::PageView page, int panel, QrUtils::OwnedPayload& out);
}  // namespace manga
