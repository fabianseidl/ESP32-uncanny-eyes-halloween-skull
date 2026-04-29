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

static void send_msg(uint8_t index, uint8_t flags) {
  EyeSyncMsg msg;
  msg.magic[0] = EYE_SYNC_MAGIC0;
  msg.magic[1] = EYE_SYNC_MAGIC1;
  msg.magic[2] = EYE_SYNC_MAGIC2;
  msg.magic[3] = EYE_SYNC_MAGIC3;
  msg.msg_type = EYE_SYNC_TYPE_GALLERY;
  msg.index    = index;
  msg.flags    = flags;
  msg.reserved = 0;

  esp_err_t r = esp_now_send(s_broadcast_addr,
                             (const uint8_t*)&msg, sizeof(msg));
#if EYE_SYNC_LOG
  Serial.printf("eye_sync: tx idx=%u flag=%s rc=%d\n",
                (unsigned)index,
                (flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb",
                (int)r);
#else
  (void)r;
#endif
}

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
  // RX drain — filled in Task 6.

  uint32_t now = millis();
  if ((uint32_t)(now - s_last_heartbeat_ms) >= EYE_SYNC_HEARTBEAT_MS) {
    send_msg(s_local_index, /*flags=*/0);
    s_last_heartbeat_ms = now;
  }
}

void eye_sync_broadcast_index(uint8_t idx) {
  if (!s_inited) {
    return;
  }
  uint32_t now            = millis();
  s_local_index           = idx;
  s_last_local_change_ms  = now;
  s_last_heartbeat_ms     = now;  // suppress immediate redundant heartbeat
  send_msg(idx, EYE_SYNC_FLAG_TAP);
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.

void eye_sync_init(void)                    {}
void eye_sync_tick(void)                    {}
void eye_sync_broadcast_index(uint8_t idx)  { (void)idx; }

#endif
