// v2a config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466).
// Eye side (mirror / convergence) is chosen at boot from STA MAC — same firmware
// on both boards. See eye_side_init() and EYE_SIDE_MAC_* below.

#pragma once

// Chunk size for async QSPI pixel pushes. Must equal ESP32QSPI_MAX_PIXELS_AT_ONCE
// (1024 default) since our transaction format (cmd=0x32 addr=0x003C00 on first
// chunk, continuation otherwise) assumes the library's per-chunk framing.
#define QSPI_ASYNC_CHUNK_PX 1024

#include "generated/eye_gallery_limits.h"

#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1

// STA MAC bytes (same order as Serial / WiFi): match → that eye; else fallback.
#define EYE_SIDE_MAC_LEFT   0x44, 0x1B, 0xF6, 0x86, 0x23, 0x20
#define EYE_SIDE_MAC_RIGHT  0x44, 0x1B, 0xF6, 0x86, 0x21, 0x80
#define EYE_SIDE_MAC_FALLBACK EYE_SIDE_RIGHT
#define EYE_SIDE_MAC_LOG      1  // 1 = one boot line with resolved side + MAC

#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466

// Physical panel = render target. Full-panel render in v2a.
#define RENDER_WIDTH  PANEL_WIDTH
#define RENDER_HEIGHT PANEL_HEIGHT

// Upscale-only guard. The Bresenham expander is correct under downscale
// too, but shipping a misconfigured downscale build is almost certainly
// unintentional -- lock it out.
static_assert(EYE_GALLERY_MAX_SCREEN_W <= RENDER_WIDTH,
              "v2a assumes source asset width <= render/panel width");
static_assert(EYE_GALLERY_MAX_SCREEN_H <= RENDER_HEIGHT,
              "v2a assumes source asset height <= render/panel height");

#define DISPLAY_BRIGHTNESS 200

// CST9217 touch support. Requires SensorLib (arduino-cli lib install SensorLib).
// Set to 0 to build without touch (serial 'n' still cycles the gallery).
#define EYE_GALLERY_HAS_TOUCH 1

// Serial diagnostics for CST9217 (down/up, coordinates). Comment out to silence.
#define EYE_GALLERY_TOUCH_LOG 0

#define TRACKING
#define AUTOBLINK
#define IRIS_SMOOTH

#if !defined(IRIS_MIN)
  #define IRIS_MIN 90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX 130
#endif

// --- Eye sync (phase C) ----------------------------------------------------
// Set EYE_SYNC_ENABLE to 0 for the single-eye fallback build (no WiFi code).
// Both boards must share the same channel value.
#define EYE_SYNC_ENABLE         1
#define EYE_SYNC_CHANNEL        1     // WiFi channel both boards use
#define EYE_SYNC_HEARTBEAT_MS   2000  // heartbeat interval per board
#define EYE_SYNC_RACE_GUARD_MS  500   // ignore inbound for this window after local tap
#define EYE_SYNC_LOG            0     // 0 = silent; 1 = serial diagnostics

// --- Eye sync phase B (animation lockstep) --------------------------------
// Requires EYE_SYNC_ENABLE 1. When 0, no anim PRNG / pulse path is compiled.
#if !defined(EYE_SYNC_ANIM_ENABLE)
#define EYE_SYNC_ANIM_ENABLE  1
#endif
#if !defined(EYE_SYNC_ANIM_PULSE_MS)
#define EYE_SYNC_ANIM_PULSE_MS  100
#endif
#if !defined(EYE_SYNC_ANIM_FALLBACK_MS)
#define EYE_SYNC_ANIM_FALLBACK_MS  4000
#endif
#if !defined(EYE_SYNC_ANIM_LOG)
#define EYE_SYNC_ANIM_LOG  0
#endif
