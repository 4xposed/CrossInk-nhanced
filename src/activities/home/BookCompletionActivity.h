#pragma once
#include "BookCompletionEdit.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class BookCompletionActivity final : public Activity {
  std::string path_, displayName_;
  BookActions::CompletionEdit edit_;
  OptionPopup retryPopup_;
  bool retryRequested_ = false;
  void attempt();
  void showRetry();

 public:
  BookCompletionActivity(GfxRenderer& renderer, MappedInputManager& input, const std::string& path,
                         const std::string& displayName)
      : Activity("BookCompletion", renderer, input), path_(path), displayName_(displayName) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // A failed edit retains its owner; the manager cancels pending intent and
  // returns control to explicit retry. Suspension/onExit never replay a save.
  bool prepareToSuspend() override { return !edit_.persistence.dirty; }
  bool cancelSuspensionOnFailure() const override { return true; }
  bool preventAutoSleep() override { return edit_.persistence.dirty; }
  bool blocksGlobalInput() const override { return true; }
  bool allowGlobalHomeGesture() const override { return false; }
};
