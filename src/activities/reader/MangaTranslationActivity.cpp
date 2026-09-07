#include "MangaTranslationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MangaTranslationActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  sdFontSystem.ensureLoaded(renderer);
  mappedInput.setReaderTouchscreenOverride(true);
  requestUpdate();
}
void MangaTranslationActivity::onExit() {
  page_ = {};
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}
void MangaTranslationActivity::loop() {
  RenderLock lock(*this);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    finish();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const bool next = touch.next || swipe == MappedInputManager::SwipeDir::Up ||
                    mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                    mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                    mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool previous = touch.prev || swipe == MappedInputManager::SwipeDir::Down ||
                        mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Up);
  if (next && more_) {
    ++pageNumber_;
    requestUpdate();
  } else if (previous && pageNumber_) {
    --pageNumber_;
    requestUpdate();
  }
}
void MangaTranslationActivity::render(RenderLock&&) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_MANGA_TRANSLATION), true);
  else
    GUI.drawHeader(renderer, header, tr(STR_MANGA_TRANSLATION), nullptr, true);
  const int font = SETTINGS.getReaderFontId();
  const int height = std::max(1, renderer.getLineHeight(font));
  const int cell = std::max(height, renderer.getTextWidth(font, "W"));
  const int top = header.y + header.height + metrics.verticalSpacing;
  const int rows = std::max(1, (safe.y + safe.height - top - height * 2) / height);
  struct DrawContext {
    GfxRenderer& renderer;
    int font, x, y, cell, height;
  };
  DrawContext context{renderer, font, safe.x, top, cell, height};
  const auto result = renderMangaTranslationPage(
      page_, scope_, pageNumber_, std::max(1, safe.width / cell), rows,
      [](void* raw, const char* glyph, int column, int row) {
        auto& c = *static_cast<DrawContext*>(raw);
        const int x = c.x + column * c.cell, y = c.y + row * c.height;
        c.renderer.beginTextClip(x, y, c.cell, c.height);
        c.renderer.drawText(c.font, x, y, glyph);
        c.renderer.endTextClip();
      },
      &context);
  more_ = result.more;
#ifdef SIMULATOR
  simulatorEmpty_ = result.empty;
#endif
  if (result.error || result.empty) {
    if (result.error) LOG_ERR("MANGA", "Cannot read stored translation scope %d", scope_);
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              result.error ? tr(STR_PAGE_LOAD_ERROR) : tr(STR_MANGA_NO_TRANSLATION));
  }
  char number[24];
  snprintf(number, sizeof(number), "%lu%s", static_cast<unsigned long>(pageNumber_ + 1), more_ ? "+" : "");
  renderer.drawCenteredText(UI_10_FONT_ID, safe.y + safe.height - height * 2, number);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", tr(STR_PREV_PAGE), tr(STR_NEXT_PAGE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
