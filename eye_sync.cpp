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

// 4-slot single-producer / single-consumer ring buffer. Producer is the
// ESP-NOW receive callback (WiFi task context); consumer is eye_sync_tick()
// on the main loop. uint8_t indices are atomic on Xtensa, so no lock.
#define EYE_SYNC_RX_QSIZE 4u
static volatile EyeSyncMsg s_rx_queue[EYE_SYNC_RX_QSIZE];
static volatile uint8_t    s_rx_head = 0;  // written by callback
static volatile uint8_t    s_rx_tail = 0;  // written by tick

// MAC of the most recent sender, captured at enqueue time. Indexed by
// the queue slot. Used only for log output.
static volatile uint8_t s_rx_mac[EYE_SYNC_RX_QSIZE][6];

static void on_recv_cb(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
  if (len != (int)sizeof(EyeSyncMsg)) {
    return;
  }
  uint8_t next = (uint8_t)((s_rx_head + 1u) % EYE_SYNC_RX_QSIZE);
  if (next == s_rx_tail) {
    // queue full — drop. We'll resync on the next heartbeat.
    return;
  }
  memcpy((void*)&s_rx_queue[s_rx_head], data, sizeof(EyeSyncMsg));
  if (info != nullptr) {
    memcpy((void*)s_rx_mac[s_rx_head], info->src_addr, 6);
  } else {
    memset((void*)s_rx_mac[s_rx_head], 0, 6);
  }
  s_rx_head = next;
}

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

  esp_now_register_recv_cb(on_recv_cb);

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
  while (s_rx_tail != s_rx_head) {
    EyeSyncMsg m;
    uint8_t    mac[6];
    memcpy(&m,  (const void*)&s_rx_queue[s_rx_tail], sizeof(EyeSyncMsg));
    memcpy(mac, (const void*)s_rx_mac[s_rx_tail],     6);
    s_rx_tail = (uint8_t)((s_rx_tail + 1u) % EYE_SYNC_RX_QSIZE);

    // Drop foreign or wrong-type traffic up front.
    if (m.magic[0] != EYE_SYNC_MAGIC0 || m.magic[1] != EYE_SYNC_MAGIC1 ||
        m.magic[2] != EYE_SYNC_MAGIC2 || m.magic[3] != EYE_SYNC_MAGIC3) {
      continue;
    }
    if (m.msg_type != EYE_SYNC_TYPE_GALLERY) {
      continue;  // reserved for phase B
    }

#if EYE_SYNC_LOG
    Serial.printf("eye_sync: rx idx=%u from=%02X:%02X:%02X:%02X:%02X:%02X flag=%s\n",
                  (unsigned)m.index,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (m.flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb");
#endif

    // Drop in-sync messages cheaply.
    if (m.index == s_local_index) {
      continue;
    }

    // Race guard: if we just tapped locally, our outbound message is in
    // flight and the peer's heartbeat may carry the OLD index. Suppress
    // applying inbound for EYE_SYNC_RACE_GUARD_MS after a local tap.
    uint32_t since_local = (uint32_t)(millis() - s_last_local_change_ms);
    if (since_local < EYE_SYNC_RACE_GUARD_MS) {
#if EYE_SYNC_LOG
      Serial.println("eye_sync:   ignore (race-guard)");
#endif
      continue;
    }

    eye_gallery_apply_remote_index(m.index);
    s_local_index = m.index;
  }

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
