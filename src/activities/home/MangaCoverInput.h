#pragma once
#include "MangaCoverWork.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"

// Drain the old batch before main-side selection/SD changes. Keep cancellation
// sticky while input callbacks run, then authorize their completed intent.
class MangaCoverInput {
 public:
  MangaCoverInput(MangaCoverWork& work, const MappedInputManager& input) : work(work) {
    using Button = MappedInputManager::Button;
    for (const auto button : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down,
                              Button::Power, Button::PageBack, Button::PageForward})
      relevant = relevant || input.wasPressed(button) || input.wasReleased(button) || input.isPressed(button);
    int x = 0, y = 0;
    relevant = relevant || input.wasScreenTouchDown(x, y) || input.wasScreenTouchReleased() ||
               input.isScreenTouchHeld(x, y) || input.wasSwipe() != MappedInputManager::SwipeDir::None;
    if (relevant) {
      intent = work.requestCancellation();
      RenderLock drain;
    }
  }
  ~MangaCoverInput() {
    if (relevant) work.authorizeIntent(intent);
  }

 private:
  MangaCoverWork& work;
  uint32_t intent = 0;
  bool relevant = false;
};
