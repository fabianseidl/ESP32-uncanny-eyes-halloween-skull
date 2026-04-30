#pragma once

#include <stdint.h>

// --- Wire format -----------------------------------------------------------
// 8-byte packed message broadcast over ESP-NOW. See
// docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md.
//
// Phase C only emits and accepts msg_type = EYE_SYNC_TYPE_GALLERY. Other
// values are reserved for phase B (animation sync) and must be ignored
// cleanly to keep the wire forward-compatible.

#define EYE_SYNC_MAGIC0  'E'
#define EYE_SYNC_MAGIC1  'Y'
#define EYE_SYNC_MAGIC2  'E'
#define EYE_SYNC_MAGIC3  '0'

#define EYE_SYNC_TYPE_GALLERY   1u
#define EYE_SYNC_TYPE_ANIM_SEED  2u
#define EYE_SYNC_TYPE_ANIM_PULSE 3u

#define EYE_SYNC_WIRE_MAX        12u

#define EYE_SYNC_FLAG_TAP      0x01u  // bit 0: tap-triggered (else heartbeat)

struct __attribute__((packed)) EyeSyncMsg {
  uint8_t magic[4];   // 'E','Y','E','0'
  uint8_t msg_type;   // EYE_SYNC_TYPE_GALLERY in phase C
  uint8_t index;      // gallery index 0..N-1
  uint8_t flags;      // bit 0 = tap-triggered
  uint8_t reserved;   // pad to 8 bytes; reserved for phase B sequence number
};
static_assert(sizeof(EyeSyncMsg) == 8, "EyeSyncMsg wire format must be 8 bytes");

struct __attribute__((packed)) EyeSyncMsgAnimSeed {
  uint8_t  magic[4];
  uint8_t  msg_type;   // EYE_SYNC_TYPE_ANIM_SEED
  uint8_t  gallery_idx;
  uint8_t  flags;
  uint8_t  reserved;
  uint32_t anim_seed;  // LE
};
static_assert(sizeof(EyeSyncMsgAnimSeed) == 12, "EyeSyncMsgAnimSeed");

struct __attribute__((packed)) EyeSyncMsgAnimPulse {
  uint8_t  magic[4];
  uint8_t  msg_type;   // EYE_SYNC_TYPE_ANIM_PULSE
  uint8_t  gallery_idx;
  uint32_t step;       // LE, wire bytes 6–9
  uint16_t reserved;   // LE, send 0
};
static_assert(sizeof(EyeSyncMsgAnimPulse) == 12, "EyeSyncMsgAnimPulse");

// --- Public API ------------------------------------------------------------
// All functions are no-ops when EYE_SYNC_ENABLE == 0.

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize WiFi STA + ESP-NOW + broadcast peer. Call once from setup()
 *  AFTER eye_gallery_init() so s_local_index can mirror the start index. */
void eye_sync_init(void);

/** Drain RX queue and (re)send heartbeat if interval elapsed.
 *  Call once per loop() iteration. Cheap: timestamp compare + (rare) queue drain. */
void eye_sync_tick(void);

/** Broadcast the current gallery index immediately. Call from
 *  eye_gallery_next() after a LOCAL advance (touch or serial). Sets the
 *  EYE_SYNC_FLAG_TAP flag and resets the race-guard timer. */
void eye_sync_broadcast_index(uint8_t idx);

#ifdef __cplusplus
}
#endif
