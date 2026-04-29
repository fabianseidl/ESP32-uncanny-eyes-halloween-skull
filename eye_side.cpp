#include <Arduino.h>

#include <esp_mac.h>
#include <string.h>

#include "config.h"
#include "eye_side.h"

uint8_t g_eye_side = EYE_SIDE_MAC_FALLBACK;

static const uint8_t k_mac_left[6]  = { EYE_SIDE_MAC_LEFT };
static const uint8_t k_mac_right[6] = { EYE_SIDE_MAC_RIGHT };

#ifndef EYE_SIDE_MAC_LOG
#define EYE_SIDE_MAC_LOG 0
#endif

void eye_side_init(void) {
  uint8_t     mac[6] = {0};
  esp_err_t   err    = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  const char* label;

  if (err != ESP_OK) {
    g_eye_side = EYE_SIDE_MAC_FALLBACK;
    label      = "fallback (esp_read_mac failed)";
  } else if (memcmp(mac, k_mac_left, 6) == 0) {
    g_eye_side = EYE_SIDE_LEFT;
    label      = "LEFT";
  } else if (memcmp(mac, k_mac_right, 6) == 0) {
    g_eye_side = EYE_SIDE_RIGHT;
    label      = "RIGHT";
  } else {
    g_eye_side = EYE_SIDE_MAC_FALLBACK;
    label      = "fallback (unknown MAC)";
  }

#if EYE_SIDE_MAC_LOG
  Serial.printf(
      "eye_side: %s mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
      label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#else
  (void)label;
#endif
}
