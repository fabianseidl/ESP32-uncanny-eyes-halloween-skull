// v2a config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466).
// Per-board settings. Flash one copy of this firmware to each board with the
// correct EYE_SIDE.

#pragma once

// Chunk size for async QSPI pixel pushes. Must equal ESP32QSPI_MAX_PIXELS_AT_ONCE
// (1024 default) since our transaction format (cmd=0x32 addr=0x003C00 on first
// chunk, continuation otherwise) assumes the library's per-chunk framing.
#define QSPI_ASYNC_CHUNK_PX 1024

#include "generated/eye_gallery_limits.h"

#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_RIGHT

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
#define EYE_GALLERY_TOUCH_LOG 1

#define TRACKING
#define AUTOBLINK
#define IRIS_SMOOTH

#if !defined(IRIS_MIN)
  #define IRIS_MIN 90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX 130
#endif
