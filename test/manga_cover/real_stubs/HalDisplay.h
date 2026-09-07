#pragma once
#include "Arduino.h"
struct TestDisplay {
  int getDisplayHeight() { return 480; }
  int getDisplayWidth() { return 800; }
};
inline TestDisplay display;
