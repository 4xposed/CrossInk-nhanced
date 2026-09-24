#pragma once

#include "MangaPageTextSource.h"
#include "activities/Activity.h"

class MangaRegionSelectionActivity final : public Activity {
 public:
  using Background = void (*)(void*);
  MangaRegionSelectionActivity(GfxRenderer& renderer, MappedInputManager& input, const manga::format::PageView& page,
                               int scope, const MangaLookupGeometry& geometry, int initial, Background background,
                               void* context)
      : Activity("MangaRegionSelection", renderer, input),
        page_(page),
        scope_(scope),
        geometry_(geometry),
        selected_(initial),
        background_(background),
        context_(context) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool blocksGlobalInput() const override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  static PageTextBounds toolbar(const GfxRenderer& renderer);

 private:
  void choose();
  manga::format::PageView page_;
  int scope_;
  MangaLookupGeometry geometry_;
  int selected_;
  Background background_;
  void* context_;
};
