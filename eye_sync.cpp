#include <Arduino.h>

#include "config.h"
#include "eye_sync.h"

#if EYE_SYNC_ENABLE

#include <WiFi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <string.h>

#include "eye_anim.h"
#include "eye_gallery.h"
#include "eye_side.h"

#ifndef EYE_SYNC_LOG
#define EYE_SYNC_LOG 0
#endif

#ifndef EYE_SYNC_ANIM_LOG
#define EYE_SYNC_ANIM_LOG 0
#endif

static const uint8_t s_broadcast_addr[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     s_inited               = false;
static uint8_t  s_local_index          = 0;
static uint32_t s_last_local_change_ms = 0;
static uint32_t s_last_heartbeat_ms    = 0;
static uint8_t  s_sta_mac[6]           = {0};

#if EYE_SYNC_ANIM_ENABLE
static uint32_t s_anim_step           = 0;
static uint32_t s_last_anim_pulse_ms  = 0;
#endif

typedef struct {
  uint8_t len;
  uint8_t mac[6];
  uint8_t payload[EYE_SYNC_WIRE_MAX];
} EyeSyncRxSlot;

#define EYE_SYNC_RX_QSIZE 8u
static volatile EyeSyncRxSlot s_rx_queue[EYE_SYNC_RX_QSIZE];
static volatile uint8_t         s_rx_head = 0;
static volatile uint8_t         s_rx_tail = 0;

static void fill_magic(uint8_t* m) {
  m[0] = EYE_SYNC_MAGIC0;
  m[1] = EYE_SYNC_MAGIC1;
  m[2] = EYE_SYNC_MAGIC2;
  m[3] = EYE_SYNC_MAGIC3;
}

#if EYE_SYNC_ANIM_ENABLE
static void send_anim_seed(uint8_t gallery_idx, uint8_t flags, uint32_t seed) {
  EyeSyncMsgAnimSeed msg;
  fill_magic(msg.magic);
  msg.msg_type    = (uint8_t)EYE_SYNC_TYPE_ANIM_SEED;
  msg.gallery_idx = gallery_idx;
  msg.flags       = flags;
  msg.reserved    = 0;
  msg.anim_seed   = seed;
  (void)esp_now_send(s_broadcast_addr, (const uint8_t*)&msg, sizeof(msg));
#if EYE_SYNC_ANIM_LOG
  Serial.printf("eye_sync: tx anim_seed idx=%u seed=0x%08X\n", (unsigned)gallery_idx,
                (unsigned)seed);
#endif
}

static void send_anim_pulse(uint8_t gallery_idx, uint32_t step) {
  EyeSyncMsgAnimPulse msg;
  fill_magic(msg.magic);
  msg.msg_type    = (uint8_t)EYE_SYNC_TYPE_ANIM_PULSE;
  msg.gallery_idx = gallery_idx;
  msg.step        = step;
  msg.reserved    = 0;
  (void)esp_now_send(s_broadcast_addr, (const uint8_t*)&msg, sizeof(msg));
#if EYE_SYNC_ANIM_LOG
  Serial.printf("eye_sync: tx anim_pulse idx=%u step=%u\n", (unsigned)gallery_idx,
                (unsigned)step);
#endif
}

static uint32_t make_anim_seed(uint8_t gallery_idx) {
  return esp_random() ^ ((uint32_t)gallery_idx << 17) ^
         ((uint32_t)s_sta_mac[5] << 8);
}

/** Left eye: new RNG epoch for iris + gaze; broadcast ANIM_SEED. */
static void leader_publish_anim_epoch(uint8_t gallery_idx, uint8_t flags) {
  uint32_t seed = make_anim_seed(gallery_idx);
  randomSeed(seed);
  eye_anim_reset_epoch(seed);
  s_anim_step = 0;
  send_anim_seed(gallery_idx, flags, seed);
}
#endif  // EYE_SYNC_ANIM_ENABLE

static void on_recv_cb(const esp_now_recv_info_t* info, const uint8_t* data,
                       int len) {
  if (info != nullptr &&
      memcmp(info->src_addr, s_sta_mac, 6) == 0) {
    return;
  }
  if (len < 8 || len > (int)EYE_SYNC_WIRE_MAX) {
    return;
  }
  uint8_t next = (uint8_t)((s_rx_head + 1u) % EYE_SYNC_RX_QSIZE);
  if (next == s_rx_tail) {
    return;
  }
  s_rx_queue[s_rx_head].len = (uint8_t)len;
  memcpy((void*)s_rx_queue[s_rx_head].payload, data, (size_t)len);
  if (info != nullptr) {
    memcpy((void*)s_rx_queue[s_rx_head].mac, info->src_addr, 6);
  } else {
    memset((void*)s_rx_queue[s_rx_head].mac, 0, 6);
  }
  s_rx_head = next;
}

static void send_msg(uint8_t index, uint8_t flags) {
  EyeSyncMsg msg;
  fill_magic(msg.magic);
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

static void drain_one_rx(const EyeSyncRxSlot* slot) {
  const uint8_t* p   = slot->payload;
  uint8_t        len = slot->len;
  uint8_t        mac[6];
  memcpy(mac, (const void*)slot->mac, 6);

  if (len < 8 || p[0] != EYE_SYNC_MAGIC0 || p[1] != EYE_SYNC_MAGIC1 ||
      p[2] != EYE_SYNC_MAGIC2 || p[3] != EYE_SYNC_MAGIC3) {
    return;
  }

  uint8_t msg_type = p[4];

  if (msg_type == EYE_SYNC_TYPE_GALLERY) {
    if (len != (int)sizeof(EyeSyncMsg)) {
      return;
    }
    EyeSyncMsg m;
    memcpy(&m, p, sizeof(EyeSyncMsg));
#if EYE_SYNC_LOG
    Serial.printf("eye_sync: rx idx=%u from=%02X:%02X:%02X:%02X:%02X:%02X flag=%s\n",
                  (unsigned)m.index, mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5], (m.flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb");
#endif
    if (m.index == s_local_index) {
      return;
    }
    uint32_t since_local = (uint32_t)(millis() - s_last_local_change_ms);
    if (since_local < EYE_SYNC_RACE_GUARD_MS) {
#if EYE_SYNC_LOG
      Serial.println("eye_sync:   ignore (race-guard)");
#endif
      return;
    }
#if EYE_SYNC_ANIM_ENABLE
    const uint8_t prev_idx = s_local_index;
#endif
    eye_gallery_apply_remote_index(m.index);
    s_local_index = m.index;
#if EYE_SYNC_ANIM_ENABLE
    if (g_eye_side == EYE_SIDE_LEFT && m.index != prev_idx) {
      leader_publish_anim_epoch(m.index,
                                (uint8_t)(m.flags & EYE_SYNC_FLAG_TAP));
    }
#endif
    return;
  }

#if EYE_SYNC_ANIM_ENABLE
  if (msg_type == EYE_SYNC_TYPE_ANIM_SEED) {
    if (len != (int)sizeof(EyeSyncMsgAnimSeed)) {
      return;
    }
    EyeSyncMsgAnimSeed m;
    memcpy(&m, p, sizeof(m));

    uint32_t since_local = (uint32_t)(millis() - s_last_local_change_ms);
    // Phase C race guard: after a local tap, ignore peer traffic that might
    // carry the *previous* gallery index. For ANIM_SEED, a matching
    // gallery_idx is the leader's epoch for the style we *just* selected on
    // touch — it must not be dropped (leader only sends once).
    if (since_local < EYE_SYNC_RACE_GUARD_MS &&
        m.gallery_idx != s_local_index) {
      return;
    }
    if (g_eye_side == EYE_SIDE_LEFT) {
      return;
    }
    if (m.gallery_idx != s_local_index) {
      eye_gallery_apply_remote_index(m.gallery_idx);
      s_local_index = m.gallery_idx;
    }
    randomSeed(m.anim_seed);
    eye_anim_reset_epoch(m.anim_seed);
#if EYE_SYNC_ANIM_LOG
    Serial.printf("eye_sync: rx anim_seed idx=%u from=%02X:%02X:...\n",
                  (unsigned)m.gallery_idx, mac[0], mac[1]);
#endif
    return;
  }

  if (msg_type == EYE_SYNC_TYPE_ANIM_PULSE) {
    if (len != (int)sizeof(EyeSyncMsgAnimPulse)) {
      return;
    }
    EyeSyncMsgAnimPulse m;
    memcpy(&m, p, sizeof(m));
    if (m.gallery_idx != s_local_index) {
      return;
    }
    if (g_eye_side == EYE_SIDE_LEFT) {
      return;
    }
    eye_anim_on_pulse(m.step);
#if EYE_SYNC_ANIM_LOG
    Serial.printf("eye_sync: rx anim_pulse step=%u\n", (unsigned)m.step);
#endif
    return;
  }
#endif  // EYE_SYNC_ANIM_ENABLE

  if (msg_type == EYE_SYNC_TYPE_BRIGHTNESS) {
    if (len != (int)sizeof(EyeSyncMsg)) {
      return;
    }
    EyeSyncMsg m;
    memcpy(&m, p, sizeof(m));
#if EYE_SYNC_LOG
    Serial.printf("eye_sync: rx brightness=%u from=%02X:%02X:...\n",
                  (unsigned)m.index, mac[0], mac[1]);
#endif
    eye_gallery_apply_remote_brightness(m.index);
    return;
  }
}


void eye_sync_init(void) {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(EYE_SYNC_CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac);

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

  esp_now_register_recv_cb(on_recv_cb);

  s_inited             = true;
  s_local_index        = 0;
  s_last_heartbeat_ms  = millis();
#if EYE_SYNC_ANIM_ENABLE
  s_last_anim_pulse_ms = millis();
  s_anim_step          = 0;
  if (g_eye_side == EYE_SIDE_LEFT) {
    leader_publish_anim_epoch(s_local_index, 0);
  }
#endif

#if EYE_SYNC_LOG
  Serial.printf("eye_sync: init ok ch=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                EYE_SYNC_CHANNEL, s_sta_mac[0], s_sta_mac[1], s_sta_mac[2],
                s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);
#endif
}

void eye_sync_tick(void) {
  if (!s_inited) {
    return;
  }
  while (s_rx_tail != s_rx_head) {
    EyeSyncRxSlot slot;
    memcpy(&slot, (const void*)&s_rx_queue[s_rx_tail], sizeof(EyeSyncRxSlot));
    s_rx_tail = (uint8_t)((s_rx_tail + 1u) % EYE_SYNC_RX_QSIZE);
    drain_one_rx(&slot);
  }

  uint32_t now = millis();
  if ((uint32_t)(now - s_last_heartbeat_ms) >= EYE_SYNC_HEARTBEAT_MS) {
    send_msg(s_local_index, /*flags=*/0);
    s_last_heartbeat_ms = now;
  }

#if EYE_SYNC_ANIM_ENABLE
  if (g_eye_side == EYE_SIDE_LEFT) {
    if ((uint32_t)(now - s_last_anim_pulse_ms) >= (uint32_t)EYE_SYNC_ANIM_PULSE_MS) {
      s_last_anim_pulse_ms = now;
      s_anim_step++;
      eye_anim_on_pulse(s_anim_step);
      send_anim_pulse(s_local_index, s_anim_step);
    }
  }
#endif
}

void eye_sync_broadcast_index(uint8_t idx) {
  if (!s_inited) {
    return;
  }
  uint32_t now           = millis();
  s_local_index          = idx;
  s_last_local_change_ms = now;
  s_last_heartbeat_ms    = now;
  send_msg(idx, EYE_SYNC_FLAG_TAP);
#if EYE_SYNC_ANIM_ENABLE
  if (g_eye_side == EYE_SIDE_LEFT) {
    leader_publish_anim_epoch(idx, EYE_SYNC_FLAG_TAP);
  }
#endif
}

void eye_sync_broadcast_brightness(uint8_t brightness) {
  if (!s_inited) {
    return;
  }
  EyeSyncMsg msg;
  fill_magic(msg.magic);
  msg.msg_type = EYE_SYNC_TYPE_BRIGHTNESS;
  msg.index    = brightness;
  msg.flags    = 0;
  msg.reserved = 0;
  (void)esp_now_send(s_broadcast_addr, (const uint8_t*)&msg, sizeof(msg));
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.

void eye_sync_init(void)                    {}
void eye_sync_tick(void)                    {}
void eye_sync_broadcast_index(uint8_t idx)  { (void)idx; }
void eye_sync_broadcast_brightness(uint8_t b) { (void)b; }

#endif
