// v1 config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466).
// Per-board settings. Flash one copy of this firmware to each board with the
// correct EYE_SIDE.

#pragma once

// Only the default_large eye is kept in v1.
#include "data/default_large.h"

// Which eye this board renders. Affects eyelid mirror direction and the
// small X-convergence offset that makes a pair look fixated.
#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_LEFT

// Physical panel.
#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466

// v1 renders the 240x240 eye natively, centered. No scaling.
#define EYE_RENDER_WIDTH   240
#define EYE_RENDER_HEIGHT  240
#define EYE_RENDER_X  ((PANEL_WIDTH  - EYE_RENDER_WIDTH ) / 2)  // 113
#define EYE_RENDER_Y  ((PANEL_HEIGHT - EYE_RENDER_HEIGHT) / 2)  // 113

// AMOLED brightness (0..255). Sent to CO5300 via command 0x51.
#define DISPLAY_BRIGHTNESS 200

// Behavior flags (retained from original sketch).
#define TRACKING      // upper lid tracks the pupil
#define AUTOBLINK     // random blinks on top of any manual blink triggers
#define IRIS_SMOOTH   // low-pass the iris scale input

// Iris range defaults -- may be overridden by the eye asset header above.
#if !defined(IRIS_MIN)
  #define IRIS_MIN  90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX  130
#endif
