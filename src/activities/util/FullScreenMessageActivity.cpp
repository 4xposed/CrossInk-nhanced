#include "FullScreenMessageActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "fontIds.h"
#include "util/BookFolderMutation.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (renderer.getScreenHeight() - height) / 2;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, top, text.c_str(), true, style);
  if (mutationRecovery) renderer.drawCenteredText(UI_10_FONT_ID, top + height * 2, tr(STR_METADATA_RECOVERY_RETRY));
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  if (!mutationRecovery) return;
  int x = 0, y = 0;
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Back) && !mappedInput.wasScreenTapped(x, y))
    return;
  const auto result = BookFolderMutation::retryPendingMutation();
  if (!BookFolderMutation::hasPending() && result != BookFolderMutation::Result::Busy) {
    LOG_INF("MUTATE", "Metadata recovery complete; restarting to finish initialization");
    ESP.restart();
  } else
    LOG_ERR("MUTATE", "Metadata recovery remains pending; explicit retry available");
}
