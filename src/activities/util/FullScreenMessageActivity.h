#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "activities/Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;
  bool mutationRecovery;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                                     const bool mutationRecovery = false)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode),
        mutationRecovery(mutationRecovery) {}
  void onEnter() override;
  void loop() override;
  bool requiresExclusiveStorageLoop() const override { return mutationRecovery; }
  bool blocksGlobalInput() const override { return mutationRecovery; }
  bool preventAutoSleep() override { return mutationRecovery; }
};
