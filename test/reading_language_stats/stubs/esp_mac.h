#pragma once
#include <cstdint>
#include <cstring>
inline bool failMac = false;
constexpr int ESP_MAC_WIFI_STA = 0, ESP_OK = 0;
inline int esp_read_mac(uint8_t* m, int) {
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  memcpy(m, mac, 6);
  return failMac ? -1 : 0;
}
inline int esp_efuse_mac_get_default(uint8_t* m) { return esp_read_mac(m, 0); }
