#pragma once
struct SettingsStub {
  int clockUtcOffsetQ = 48;
};
inline SettingsStub testSettings;
#define SETTINGS testSettings
