#pragma once
#include <cstdint>
struct ClockStub {
  bool available = false;
  uint16_t year = 2026;
  uint8_t month = 9, day = 7;
  bool getDateTime(uint16_t& y, uint8_t& m, uint8_t& d, uint8_t& h, uint8_t& minute) {
    y = year;
    m = month;
    d = day;
    h = 12;
    minute = 0;
    return available;
  }
};
inline ClockStub halClock;
