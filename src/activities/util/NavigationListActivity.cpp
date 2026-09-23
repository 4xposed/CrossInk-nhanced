#include "NavigationListActivity.h"

#include <array>

#include "components/TouchHeaderBackButton.h"
#include "components/UiAppHelpers.h"
namespace fui = freeink::ui;
NavigationListActivity::NavigationListActivity(const char* name, StrId title, GfxRenderer& renderer,
                                               MappedInputManager& input)
    : Activity(name, renderer, input),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()),
      title(title) {}
void NavigationListActivity::onEnter() {
  Activity::onEnter();
  applySharedUiTheme(app, uiTarget);
  app.on(1, onRow, this);
  app.setScreen(screen, this);
  requestUpdate();
}
void NavigationListActivity::onRow(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<NavigationListActivity*>(user);
  if (event.value < 0 || event.value >= self.itemCount()) return;
  self.selected = event.value;
  self.app.clearTapFlash();
  self.pendingSelection = event.value;
}
void NavigationListActivity::loop() {
  if ((showBack() && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    back();
    return;
  }
  bool activated = false;
  const auto snap = touchSnapshotFrom(mappedInput);
  if (snap.touchPressed || snap.touchReleased) {
    RenderLock lock(*this);
    activated = static_cast<bool>(app.route(snap));
    if (app.invalidated()) requestUpdate();
  }
  if (activated) {
    const int choice = pendingSelection;
    pendingSelection = -1;
    if (choice >= 0) activate(choice);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected);
    return;
  }
  const auto move = [this](int delta) {
    RenderLock lock(*this);
    const int count = itemCount();
    const int first = firstSelection();
    if (count > first) selected = first + (selected - first + count - first + delta) % (count - first);
    top = followListSelection(selected, top, visibleRows, count);
    requestUpdate();
  };
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    RenderLock lock(*this);
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    selected = std::clamp(selected + delta, 0, itemCount() - 1);
    top = followListSelection(selected, top, visibleRows, itemCount());
    requestUpdate();
    return;
  }
  navigator.onNextRelease([&] { move(1); });
  navigator.onPreviousRelease([&] { move(-1); });
}
void NavigationListActivity::screen(UiApp::ScreenType& screen, void* user) {
  auto& self = *static_cast<NavigationListActivity*>(user);
  int insetTop, insetRight, insetBottom, insetLeft;
  self.renderer.getOrientedViewableTRBL(&insetTop, &insetRight, &insetBottom, &insetLeft);
  const auto safe = UITheme::getInstance().getScreenSafeArea(self.renderer, true, true);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(std::max(self.contentTop, std::max(safe.y, insetTop))),
                  static_cast<int16_t>(std::max(insetRight, self.renderer.getScreenWidth() - safe.x - safe.width)),
                  static_cast<int16_t>(std::max(insetBottom, self.renderer.getScreenHeight() - safe.y - safe.height)),
                  static_cast<int16_t>(std::max(insetLeft, safe.x))});
  auto& items = self.rows;
  items.fill({});
  const int count = std::min(self.itemCount(), static_cast<int>(items.size()));
  for (int i = 0; i < count; ++i) {
    items[i].label = self.itemLabel(i);
    items[i].icon = self.itemIcon(i);
    items[i].subtitle = self.itemValue(i);
    items[i].actionValue = i;
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = count;
  props.selectedIndex = self.selected;
  props.action = 1;
  props.inputMask = fui::InputTouch;
  self.visibleRows = std::max(1, static_cast<int>(configureUiList(
                                     props, screen.theme(), screen.body(),
                                     self.itemValue(0) ? UiListRowType::WithSubtitle : UiListRowType::SingleLine)));
  self.top = followListSelection(self.selected, self.top, self.visibleRows, count);
  props.topIndex = self.top;
  screen.list(props);
}
void NavigationListActivity::present() { renderer.displayBuffer(); }
void NavigationListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto rect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (showBack() && mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, uiTarget, rect, I18N.get(title), false);
  else
    GUI.drawHeader(renderer, rect, I18N.get(title));
  contentTop = rect.y + rect.height + drawAboveList();
  app.render();
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawFeedback();
  present();
}
