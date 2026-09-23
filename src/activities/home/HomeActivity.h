#pragma once
#include <HalDisplay.h>

#include "MangaCoverWork.h"
#include "RecentBooksStore.h"
#include "activities/util/NavigationListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
class HomeActivity final : public NavigationListActivity {
 public:
  HomeActivity(GfxRenderer& renderer, MappedInputManager& input, HomeMenuItem menu = HomeMenuItem::NONE,
               HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH, std::string bookPath = {})
      : NavigationListActivity("Home", StrId::STR_HOME, renderer, input),
        initialMenu(menu),
        refreshMode(refreshMode),
        initialBookPath(std::move(bookPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  std::string getCurrentBookPath() const override { return book.path; }
  std::string getCurrentBookTitle() const override { return book.title; }
  std::unique_ptr<Activity> createFrontlightReadingStatsActivity() override;
  void onFrontlightPanelClosed() override;
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;
  void requestBackgroundCancellation() override { coverWork.requestCancellation(); }
  bool prepareToSuspend() override { return !coverWork.active; }
  void onResume() override { coverWork.authorizeIntent(); }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;

 private:
  HomeMenuItem initialMenu;
  // Silent restarts keep the panel's previous frame. The first Home paint may
  // need a clean waveform so X4 panels do not diff against a WiFi screen.
  HalDisplay::RefreshMode refreshMode;
  bool prepared = false, backPressed = false, quickActionsLongPowerHandled = false;
  std::string initialBookPath;
  RecentBook book;
  Rect preview, cover;
  MangaCoverWork coverWork;
  OptionPopup quickActionsPopup;
  int itemCount() const override { return 6; }
  int firstSelection() const override { return book.path.empty() ? 0 : -1; }
  const char* itemLabel(int index) const override;
  void activate(int index) override;
  void back() override;
  bool showBack() const override { return false; }
  int drawAboveList() override;
  void present() override;
  void renderTouchHome();
  void loopTouchHome();
  bool touchMenuOpen = false;
  bool touchButtonFocus = false;
  float readingPercent = -1.0f;
  Rect lightButton, menuButton, continueButton, libraryButton, ankiButton, menuRows[4];
};
