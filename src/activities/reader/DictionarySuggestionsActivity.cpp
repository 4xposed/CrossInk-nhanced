#include "DictionarySuggestionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/DictionaryActivityUtils.h"

namespace fui = freeink::ui;

DictionarySuggestionsActivity::DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::vector<std::string> suggestions)
    : Activity("DictionarySuggestions", renderer, mappedInput),
      suggestions(std::move(suggestions)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

DictionarySuggestionsActivity::DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             DictionarySuggestions suggestions)
    : Activity("DictionarySuggestions", renderer, mappedInput),
      boundedSuggestions_(std::move(suggestions)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

size_t DictionarySuggestionsActivity::suggestionCount() const {
  return suggestions.empty() ? boundedSuggestions_.count : suggestions.size();
}

const char* DictionarySuggestionsActivity::suggestionAt(const size_t index) const {
  if (index >= suggestionCount()) return "";
  return suggestions.empty() ? boundedSuggestions_.items[index].c_str() : suggestions[index].c_str();
}

void DictionarySuggestionsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &DictionarySuggestionsActivity::onRowEvent, this);
  app.setScreen(&DictionarySuggestionsActivity::suggestionsScreen, this);
  uiItemCount_ = static_cast<uint8_t>(std::min<size_t>(suggestionCount(), uiItems_.size()));
  for (uint8_t i = 0; i < uiItemCount_; ++i) {
    uiItems_[i].label = suggestionAt(i);
    uiItems_[i].actionValue = static_cast<int16_t>(i);
  }
  requestUpdate();
}

void DictionarySuggestionsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<DictionarySuggestionsActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->suggestionCount())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->setResult(WordResult{self->suggestionAt(self->selectedIndex)});
  self->finish();
}

void DictionarySuggestionsActivity::suggestionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<DictionarySuggestionsActivity*>(user)->buildSuggestionsScreen(screen);
}

void DictionarySuggestionsActivity::buildSuggestionsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.verticalSpacing),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  fui::ListProps props;
  props.items = uiItems_.data();
  props.count = uiItemCount_;
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.topIndex = static_cast<uint16_t>(topIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(suggestionCount()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void DictionarySuggestionsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, static_cast<int>(suggestionCount()));
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(WordResult{suggestionAt(selectedIndex)});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
  const bool prevItem = DictUtils::dictionaryPageButtonTriggered(mappedInput, true) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextItem = DictUtils::dictionaryPageButtonTriggered(mappedInput, false) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (prevItem && selectedIndex > 0) {
    selectedIndex--;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(suggestionCount()));
    requestUpdate();
  }
  if (nextItem && selectedIndex < static_cast<int>(suggestionCount()) - 1) {
    selectedIndex++;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(suggestionCount()));
    requestUpdate();
  }
}

void DictionarySuggestionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_DICT_DID_YOU_MEAN), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_DICT_DID_YOU_MEAN));
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
