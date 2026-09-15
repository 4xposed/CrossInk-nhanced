#include "MangaRegionSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MangaRegionSelectionActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  requestUpdate();
}

void MangaRegionSelectionActivity::onExit() {
  page_ = {};
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

PageTextBounds MangaRegionSelectionActivity::toolbar(const GfxRenderer& renderer) {
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int height = std::min(safe.height, std::max(36, renderer.getLineHeight(UI_10_FONT_ID) + 12));
  return {int16_t(safe.x), int16_t(safe.y + safe.height - height), int16_t(safe.width), int16_t(height)};
}

void MangaRegionSelectionActivity::choose() {
  PageTextBounds bounds;
  if (!mangaLookupRegionBounds(page_, scope_, geometry_, selected_, bounds)) return;
  setResult(MenuResult{selected_});
  finish();
}

void MangaRegionSelectionActivity::loop() {
  RenderLock lock(*this);
  int x = 0, y = 0;
  int command = -1;
  if (mappedInput.wasScreenTapped(x, y)) {
    const auto bar = toolbar(renderer);
    if (x >= bar.x && x < bar.x + bar.width && y >= bar.y && y < bar.y + bar.height) {
      command = (x - bar.x) * 4 / bar.width;
    } else {
      const int hit = mangaLookupRegionAtPoint(page_, scope_, geometry_, x, y);
      if (hit >= 0) {
        if (hit == selected_) {
          choose();
          return;
        }
        selected_ = hit;
        requestUpdate();
      }
      return;
    }
  }
  if (command == 0 || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (command == 1 || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    choose();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  const bool previous = command == 2 || swipe == MappedInputManager::SwipeDir::Down ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                        mappedInput.wasReleased(MappedInputManager::Button::PageBack);
  const bool next = command == 3 || swipe == MappedInputManager::SwipeDir::Up ||
                    mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                    mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                    mappedInput.wasReleased(MappedInputManager::Button::PageForward);
  if (previous || next) {
    selected_ = nextMangaLookupRegion(page_, scope_, geometry_, selected_, !previous);
    requestUpdate();
  }
}

void MangaRegionSelectionActivity::render(RenderLock&&) {
  background_(context_);
  PageTextBounds bounds;
  if (mangaLookupRegionBounds(page_, scope_, geometry_, selected_, bounds)) {
    renderer.drawRect(bounds.x, bounds.y, bounds.width, bounds.height, 3, false);
    renderer.drawRect(bounds.x, bounds.y, bounds.width, bounds.height, 1, true);
  }
  const auto bar = toolbar(renderer);
  renderer.fillRect(bar.x, bar.y, bar.width, bar.height, false);
  renderer.drawRect(bar.x, bar.y, bar.width, bar.height);
  const char* labels[] = {tr(STR_BACK), tr(STR_LOOKUP_SHORT), tr(STR_PREV), tr(STR_NEXT)};
  for (int i = 0; i < 4; ++i) {
    const int left = bar.x + bar.width * i / 4;
    const int right = bar.x + bar.width * (i + 1) / 4;
    if (i) renderer.drawLine(left, bar.y, left, bar.y + bar.height - 1, true);
    renderer.beginTextClip(left + 2, bar.y + 2, right - left - 4, bar.height - 4);
    renderer.drawText(UI_10_FONT_ID, left + (right - left - renderer.getTextWidth(UI_10_FONT_ID, labels[i])) / 2,
                      bar.y + (bar.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, labels[i]);
    renderer.endTextClip();
  }
  renderer.displayBuffer();
}
