#pragma once
#include "activities/util/NavigationListActivity.h"
class LibraryFoldersActivity final : public NavigationListActivity {
 public:
  LibraryFoldersActivity(GfxRenderer& renderer, MappedInputManager& input)
      : NavigationListActivity("LibraryFolders", StrId::STR_LIBRARY_FOLDERS, renderer, input) {}

 private:
  int itemCount() const override { return 3; }
  const char* itemLabel(int index) const override;
  const char* itemValue(int index) const override;
  void activate(int index) override;
  void drawFeedback() override;
  char* folder(int index) const;
  bool saveFailed = false;
};
