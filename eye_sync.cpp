#include <Arduino.h>

#include "config.h"
#include "eye_sync.h"

#if EYE_SYNC_ENABLE

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "eye_gallery.h"

#ifndef EYE_SYNC_LOG
#define EYE_SYNC_LOG 0
#endif

static const uint8_t s_broadcast_addr[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     s_inited                 = false;
static uint8_t  s_local_index            = 0;
static uint32_t s_last_local_change_ms   = 0;
static uint32_t s_last_heartbeat_ms      = 0;

void eye_sync_init(void) {
  WiFi.mode(WIFI_STA);
  // Channel must be set before esp_now_init() so the radio is parked
  // where both boards expect it. We never associate with an AP, so the
  // channel will not drift.
  esp_wifi_set_channel(EYE_SYNC_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
#if EYE_SYNC_LOG
    Serial.println("eye_sync: esp_now_init FAILED");
#endif
    return;
  }

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, s_broadcast_addr, 6);
  peer.channel = EYE_SYNC_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
#if EYE_SYNC_LOG
    Serial.println("eye_sync: add_peer FAILED");
#endif
    return;
  }

  s_inited            = true;
  s_local_index       = 0;  // gallery starts at 0; tap broadcasts will update.
  s_last_heartbeat_ms = millis();

#if EYE_SYNC_LOG
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("eye_sync: init ok ch=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                EYE_SYNC_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

void eye_sync_tick(void) {
  if (!s_inited) {
    return;
  }
  // RX drain + heartbeat — filled in Tasks 5 and 6.
}

void eye_sync_broadcast_index(uint8_t idx) {
  (void)idx;
  // Filled in Task 5.
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.

void eye_sync_init(void)                    {}
void eye_sync_tick(void)                    {}
void eye_sync_broadcast_index(uint8_t idx)  { (void)idx; }

#endif
