#pragma once
#include <array>
#include <memory>

#include "LibraryNavigation.h"
#include "MangaCoverWork.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"
#include "util/LibraryCatalog.h"
class LibraryActivity final : public Activity {
 public:
  LibraryActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("Library", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void requestBackgroundCancellation() override { coverWork.requestCancellation(); }
  bool prepareToSuspend() override { return !coverWork.active; }
  void onResume() override { coverWork.authorizeIntent(); }

 private:
  std::unique_ptr<library::LibraryCatalog> catalog;
  std::unique_ptr<library::CatalogEntry> entry;
  std::array<RecentBook, 9> books;
  std::array<bool, 9> prepared{};
  std::array<Rect, 9> cells;
  std::vector<TabInfo> tabs;
  std::array<library::Category, 4> categories{};
  library::Navigation navigation;
  library::GridLayout grid{1, 1, 1, 1, 1, 1};
  Rect tabRect, body;
  MangaCoverWork coverWork;
  ButtonNavigator navigator;
  int selectedTab = 0, pageSize = 0;
  uint32_t loadedPage = UINT32_MAX;
  bool longPressHandled = false, actionFailed = false;
  void rebuild();
  void updateTabs();
  void changeTab(int delta);
  void openSelected();
  void showActions();
  void runAction(int action, const RecentBook& book);
};
