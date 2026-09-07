#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

QrDisplayActivity::QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& text)
    : Activity("QrDisplay", renderer, mappedInput) {
  payload.length = text.size() > QrUtils::kMaxPayloadBytes
                       ? utf8SafeTruncateBuffer(text.c_str(), QrUtils::kMaxPayloadBytes)
                       : text.size();
  payload.truncated = payload.length < text.size();
  // Compatibility caller: own a bounded copy across the stacked child lifetime.
  payload.bytes = makeUniqueNoThrow<char[]>(payload.length + 1);
  if (payload.bytes)
    std::memcpy(payload.bytes.get(), text.data(), payload.length);
  else
    LOG_ERR("QR", "Cannot allocate QR payload");
}

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  moduleCapacity = QrUtils::gridBytesForPayload(payload.length);
  // <=3917 bytes, reused across redraws. Encode only in render(), whose task has
  // the existing 16384-byte stack; onEnter runs on a different, smaller stack.
  bool allocate = moduleCapacity && payload.bytes;
#ifdef SIMULATOR
  allocate = allocate && !failModulesForTest;
  failModulesForTest = false;
#endif
  if (allocate) modules = makeUniqueNoThrow<uint8_t[]>(moduleCapacity);
  if (!modules) LOG_ERR("QR", "Cannot allocate QR module grid (%u bytes)", static_cast<unsigned>(moduleCapacity));
  requestUpdate();
}

void QrDisplayActivity::onExit() {
  modules.reset();
  payload = {};
  Activity::onExit();
}

void QrDisplayActivity::loop() {
  int x = 0;
  int y = 0;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_DISPLAY_QR), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_DISPLAY_QR), nullptr);
  }

  const int availableWidth = pageWidth - 40;
  const int availableHeight = pageHeight - metrics.topPadding - TouchHeaderBackButton::height(metrics, mappedInput) -
                              metrics.verticalSpacing * 2 - 40;
  const int startY = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;

  const Rect qrBounds(20, startY, availableWidth, availableHeight);
  const auto result = modules ? QrUtils::drawQrCode(renderer, qrBounds, payload.bytes.get(), payload.length,
                                                    modules.get(), moduleCapacity)
                              : QrUtils::DrawResult::OutOfMemory;
  if (result != QrUtils::DrawResult::Drawn) {
    renderer.drawCenteredText(
        UI_10_FONT_ID, pageHeight / 2,
        I18n::getInstance().get(result == QrUtils::DrawResult::OutOfMemory ? StrId::STR_MEMORY_ERROR
                                                                           : StrId::STR_PAGE_LOAD_ERROR));
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
