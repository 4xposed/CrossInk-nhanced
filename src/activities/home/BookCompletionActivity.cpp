#include "BookCompletionActivity.h"

#include <I18n.h>

#include "BookActions.h"

void BookCompletionActivity::onEnter() {
  Activity::onEnter();
  attempt();
}

void BookCompletionActivity::showRetry() {
  RenderLock lock(*this);
  const char* options[] = {tr(STR_RETRY)};
  retryPopup_.show(tr(STR_STATS_SAVE_FAILED), options, 1, 0, [this](int) { retryRequested_ = true; });
  retryPopup_.setPrimaryOptionIndex(0);
  requestUpdate();
}

void BookCompletionActivity::attempt() {
  bool completed = false;
  if (BookActions::toggleBookCompleted(path_, displayName_, completed, edit_)) {
    setResult(OptionSelectionResult{static_cast<uint8_t>(completed)});
    finish();
    return;
  }
  showRetry();
}

void BookCompletionActivity::loop() {
  bool handled;
  {
    RenderLock lock(*this);
    handled = retryPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
  }
  if (retryRequested_) {
    retryRequested_ = false;
    attempt();
  } else if (!handled) {
    if (edit_.persistence.dirty) {
      showRetry();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
  }
}

void BookCompletionActivity::render(RenderLock&&) { retryPopup_.processRender(renderer, mappedInput); }
