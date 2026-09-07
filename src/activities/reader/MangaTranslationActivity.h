#pragma once
#include <FreeInkUIGfxRenderer.h>

#include "MangaTranslationPager.h"
#include "activities/Activity.h"
#include "components/UiAppHelpers.h"

// Parent owns immutable PageView bytes until this foreground child exits.
class MangaTranslationActivity final : public Activity {
 public:
  MangaTranslationActivity(GfxRenderer& renderer, MappedInputManager& input, manga::format::PageView page, int scope)
      : Activity("MangaTranslation", renderer, input), page_(page), scope_(scope), uiTarget_(makeUiTarget(renderer)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
#ifdef SIMULATOR
  bool simulatorEmpty() {
    RenderLock lock(*this);
    return simulatorEmpty_;
  }
  uint32_t simulatorPage() {
    RenderLock lock(*this);
    return pageNumber_;
  }
#endif

 private:
  manga::format::PageView page_;
  int scope_;
  uint32_t pageNumber_ = 0;
  bool more_ = false;
  freeink::ui::GfxRendererTarget uiTarget_;
#ifdef SIMULATOR
  bool simulatorEmpty_ = true;
#endif
};
