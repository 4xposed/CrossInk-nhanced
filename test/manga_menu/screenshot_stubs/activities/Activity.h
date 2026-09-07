#pragma once
#include "ScreenshotInfo.h"
struct ScreenshotActivityStub {
  ScreenshotInfo getScreenshotInfo() const { return {}; }
};
inline ScreenshotActivityStub activityManager;
