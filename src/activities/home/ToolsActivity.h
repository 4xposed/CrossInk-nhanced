#pragma once
#include "activities/util/NavigationListActivity.h"
class ToolsActivity final : public NavigationListActivity {
 public:
  ToolsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : NavigationListActivity("Tools", StrId::STR_TOOLS, renderer, input) {}

 private:
  int itemCount() const override { return 4; }
  const char* itemLabel(int index) const override;
  void activate(int index) override;
  void back() override { onGoHome(HomeMenuItem::TOOLS); }
};
