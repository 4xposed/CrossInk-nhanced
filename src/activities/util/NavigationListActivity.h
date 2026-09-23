#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <array>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Small app menus share one list configuration for drawing, scrolling and touch.
class NavigationListActivity : public Activity {
 protected:
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator navigator;
  int selected = 0;
  int top = 0;
  int visibleRows = 1;
  virtual int firstSelection() const { return 0; }
  virtual int itemCount() const = 0;
  virtual const char* itemLabel(int index) const = 0;
  virtual freeink::ui::BitmapRef itemIcon(int) const { return {}; }
  virtual const char* itemValue(int) const { return nullptr; }
  virtual void activate(int index) = 0;
  virtual void back() { finish(); }
  virtual int drawAboveList() { return 0; }
  virtual void drawFeedback() {}
  virtual bool showBack() const { return true; }
  virtual void present();
  StrId title;

 public:
  NavigationListActivity(const char* name, StrId title, GfxRenderer&, MappedInputManager&);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void screen(UiApp::ScreenType&, void*);
  static void onRow(const freeink::ui::ActionEvent&, void*);
  // SDK rows include icon/style data; keep seven rows on the activity heap, not the render stack.
  std::array<freeink::ui::ListItem, 7> rows{};
  int contentTop = 0;
  int pendingSelection = -1;
};
