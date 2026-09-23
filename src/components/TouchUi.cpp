#include "TouchUi.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <cstdio>

#include "AppCapabilities.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace TouchUi {
int titleFont() {
  return SETTINGS.uiScale == CrossPointSettings::UI_SCALE_SMALL ? BITTER_14_FONT_ID : BITTER_16_FONT_ID;
}
int bodyFont() { return SETTINGS.uiScale == CrossPointSettings::UI_SCALE_LARGE ? UI_12_FONT_ID : UI_10_FONT_ID; }
int smallFont() { return SETTINGS.uiScale == CrossPointSettings::UI_SCALE_LARGE ? UI_10_FONT_ID : SMALL_FONT_ID; }
#if CROSSINK_APP_DEVICE_X4PRO
bool enabled() { return gpio.hasTouch(); }
bool enabled(const MappedInputManager& input) { return input.hasTouchHardware(); }
#endif
int statusHeight(const GfxRenderer& renderer) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  return top + 8 + renderer.getLineHeight(SMALL_FONT_ID) + 8;
}
#if CROSSINK_APP_DEVICE_X4PRO
void drawStatus(const GfxRenderer& renderer, const bool darkMode) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const bool ink = !darkMode;
  // Image readers and drawer previews can paint beneath this shared lane.
  renderer.fillRect(left, top, renderer.getScreenWidth() - left - right, statusHeight(renderer) - top, darkMode);
  const int y = top + 8;
  char time[9] = {};
  if (halClock.isAvailable() &&
      halClock.formatTime(time, sizeof(time), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    renderer.drawText(SMALL_FONT_ID, left + 16, y, time, ink);
  }
  const unsigned percent = std::min<unsigned>(100, powerManager.getBatteryPercentage());
  char text[8];
  snprintf(text, sizeof(text), "%u%%", percent);
  constexpr int iconWidth = 22;
  constexpr int iconHeight = 12;
  const int x = renderer.getScreenWidth() - right - 16 - iconWidth;
  renderer.drawText(SMALL_FONT_ID, x - 8 - renderer.getTextWidth(SMALL_FONT_ID, text), y, text, ink);
  const int iconY = y + (renderer.getLineHeight(SMALL_FONT_ID) - iconHeight) / 2;
  renderer.drawRect(x, iconY, iconWidth - 3, iconHeight, ink);
  renderer.fillRect(x + iconWidth - 3, iconY + 4, 3, 4, ink);
  const int fill = (iconWidth - 7) * percent / 100;
  if (fill > 0) renderer.fillRect(x + 2, iconY + 2, fill, iconHeight - 4, ink);
}
#endif
}  // namespace TouchUi
