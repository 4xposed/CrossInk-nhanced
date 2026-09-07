#pragma once
#include <cstdint>

namespace manga {
enum class MenuAction : uint8_t {
  Chapter,
  Percent,
  Bookmarks,
  ToggleBookmark,
  PanelsOnly,
  PanelRotation,
  Orientation,
  Home,
  Lookup,
  Translation,
  LookupHistory,
  ReaderSettings,
  AutoTurn,
  Screenshot,
  DeleteCache,
  OcrQr,
  None,
  Dismiss
};
constexpr int kMenuActionCount = 16;
constexpr MenuAction menuActionAt(int index) {
  return index >= 0 && index < kMenuActionCount ? static_cast<MenuAction>(index) : MenuAction::None;
}

// One deadline, never a backlog. A due/busy tick waits for idle, then grants a
// whole interval. A successful move waits for its completed display to re-arm.
class AutoTurn {
 public:
  void select(int index, uint32_t now) {
    rate_ = index > 0 && index < 5 ? static_cast<uint8_t>(index) : 0;
    deadline_ = now + intervalMs();
    waitingIdle_ = waitingRender_ = false;
  }
  void cancel() {
    rate_ = 0;
    waitingIdle_ = waitingRender_ = false;
  }
  bool active() const { return rate_ != 0; }
  uint8_t rateIndex() const { return rate_; }
  uint32_t intervalMs() const {
    constexpr uint32_t intervals[] = {0, 60000, 20000, 10000, 5000};
    return intervals[rate_];
  }
  bool poll(uint32_t now, bool idle) {
    if (!active() || waitingRender_) return false;
    if (waitingIdle_) {
      if (idle) {
        waitingIdle_ = false;
        deadline_ = now + intervalMs();
      }
      return false;
    }
    if (static_cast<int32_t>(now - deadline_) < 0) return false;
    if (!idle) {
      waitingIdle_ = true;
      return false;
    }
    waitingRender_ = true;
    return true;
  }
  void rendered(uint32_t now) {
    if (active() && (waitingRender_ || waitingIdle_ || static_cast<int32_t>(now - deadline_) >= 0)) {
      waitingRender_ = false;
      waitingIdle_ = false;
      deadline_ = now + intervalMs();
    }
  }

 private:
  uint32_t deadline_ = 0;
  uint8_t rate_ = 0;
  bool waitingIdle_ = false, waitingRender_ = false;
};
}  // namespace manga
