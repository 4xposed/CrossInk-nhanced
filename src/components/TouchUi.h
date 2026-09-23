#pragma once

#include "AppCapabilities.h"

class GfxRenderer;
class MappedInputManager;

// Shared chrome for the approved X4 Pro touch screens. Inline false gates let
// button-only firmware discard the presentation and its translated strings.
namespace TouchUi {
#if CROSSINK_APP_DEVICE_X4PRO
bool enabled();
bool enabled(const MappedInputManager& input);
void drawStatus(const GfxRenderer& renderer, bool darkMode = false);
#else
inline constexpr bool enabled() { return false; }
inline constexpr bool enabled(const MappedInputManager&) { return false; }
inline void drawStatus(const GfxRenderer&, bool = false) {}
#endif
// Absolute bottom of the status lane, including the oriented bezel inset.
int statusHeight(const GfxRenderer& renderer);
// Distinct text roles for the mockup's serif headings and compact sans labels.
// Reuse built-in fonts; the large UI preference still enlarges interactive text.
int titleFont();
int bodyFont();
int smallFont();
}  // namespace TouchUi
