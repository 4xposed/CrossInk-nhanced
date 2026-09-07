#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "util/QrCodePolicy.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& textPayload);
  QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, QrUtils::OwnedPayload&& payload)
      : Activity("QrDisplay", renderer, mappedInput), payload(std::move(payload)) {}

#ifdef SIMULATOR
  static void simulatorFailAllocation(bool modules) {
    failActivityForTest = !modules;
    failModulesForTest = modules;
  }
  static bool simulatorTakeActivityFailure() {
    const bool fail = failActivityForTest;
    failActivityForTest = false;
    return fail;
  }
  size_t simulatorPayloadBytes() const { return payload.length; }
  size_t simulatorModuleBytes() const { return modules ? moduleCapacity : 0; }
#endif
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
#ifdef SIMULATOR
  inline static bool failActivityForTest = false, failModulesForTest = false;
#endif
  QrUtils::OwnedPayload payload;
  std::unique_ptr<uint8_t[]> modules;
  size_t moduleCapacity = 0;
};
