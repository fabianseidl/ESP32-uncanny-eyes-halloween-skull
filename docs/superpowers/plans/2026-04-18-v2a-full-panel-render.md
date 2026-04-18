# v2a — Full-Panel 466×466 Render Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scale the renderer so the eye fills the full 466×466 CO5300 AMOLED, using the existing 240-baked `default_large` asset stretched nearest-neighbor via a size-agnostic scanline loop.

**Architecture:** Separate source-space (asset dims, from `default_large.h`) from render-space (panel dims, from `config.h`). The scanline loop iterates in render-space and reads source-space via per-axis Bresenham integer accumulators. Collapses to identity when source == render, so a future native-466 asset swaps in with zero renderer change.

**Tech Stack:** Same as v1 — Arduino IDE 2.x, ESP32 core ≥ 3.0, `Arduino_GFX` (moononournation), `data/default_large.h` (unchanged).

**Testing model:** Microcontroller sketch. "Tests" are: (a) `arduino-cli compile` or Arduino IDE **Verify** produces no errors/warnings, and (b) flashing to the Waveshare board produces the expected visual output and FPS. Each code task ends with a compile check; the acceptance task adds a flash-and-look + FPS measurement.

**Reference spec:** `docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md`

---

## File Structure After Implementation

No files created or deleted; only the following four are modified:

```
config.h                   drop EYE_RENDER_*, add RENDER_WIDTH / RENDER_HEIGHT
eye_functions.ino          drawEye() rewritten with render-space outer loops +
                           Bresenham source-index advance; frame() / updateEye() /
                           split() unchanged
README.md                  wording updated from "240×240 centered" to
                           "full-panel 466×466 NN-stretched"
docs/superpowers/plans/2026-04-18-v2a-full-panel-render.md   (this file)
```

Unchanged: `ESP32-uncanny-eyes-halloween-skull.ino`, `display.ino`, `data/default_large.h`, `LICENSE`, `docs/hardware-notes.md`, `tools/hello_amoled/`.

---

## Arduino IDE board settings (same as v1, verify once)

| Menu                    | Value                                           |
| ----------------------- | ----------------------------------------------- |
| Board                   | ESP32S3 Dev Module (`esp32:esp32:esp32s3`)      |
| USB CDC On Boot         | Enabled                                         |
| USB Mode                | Hardware CDC and JTAG                           |
| CPU Frequency           | 240MHz (WiFi)                                   |
| Flash Mode              | QIO 80MHz                                       |
| Flash Size              | 16MB (128Mb)                                    |
| PSRAM                   | OPI PSRAM                                       |
| Partition Scheme        | Default 4MB with spiffs (or 16M Flash 3MB app)  |
| Upload Speed            | 921600                                          |

Equivalent `arduino-cli` FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default`

---

## Task 1: Update `config.h` — render-space knobs, drop centered-inset knobs

**Rationale:** The renderer rewrite in Task 2 needs `RENDER_WIDTH` / `RENDER_HEIGHT` to exist, and it will no longer reference `EYE_RENDER_*`. Landing the config change first keeps the commit history reviewable (config delta is tiny) and means Task 2's diff is purely renderer logic.

Note: after this task, the project **will not compile** — `eye_functions.ino` still references `EYE_RENDER_X/Y/WIDTH/HEIGHT`. Task 2 restores compilability. Do not run a compile check between Task 1 and Task 2.

**Files:**
- Modify (overwrite): `config.h`

- [ ] **Step 1: Overwrite `config.h`**

```cpp
// v2a config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466).
// Full-panel render: the 240-baked default_large asset is stretched NN to
// the full 466x466 panel in drawEye() via Bresenham source mapping.
// See docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md.

#pragma once

// Source-space constants (SCREEN_WIDTH/HEIGHT, SCLERA_*, IRIS_*, IRIS_MAP_*)
// come from the asset header.
#include "data/default_large.h"

// Which eye this board renders. Affects eyelid mirror direction and the
// small X-convergence offset that makes a pair look fixated.
#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_LEFT

// Physical panel = render target. v2a fills it completely.
#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466
#define RENDER_WIDTH  PANEL_WIDTH
#define RENDER_HEIGHT PANEL_HEIGHT

// v2a assumes the asset is no larger than the panel in either axis.
// (Downscale would still be correct math but is out of scope.)
static_assert(SCREEN_WIDTH  <= RENDER_WIDTH,
              "v2a renderer assumes SCREEN_WIDTH <= RENDER_WIDTH");
static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT,
              "v2a renderer assumes SCREEN_HEIGHT <= RENDER_HEIGHT");

// AMOLED brightness (0..255). Sent to CO5300 via command 0x51.
#define DISPLAY_BRIGHTNESS 200

// Behavior flags (retained from v1).
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
```

Changes vs v1 `config.h`:
- **Deleted:** `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`, `EYE_RENDER_X`, `EYE_RENDER_Y`.
- **Added:** `RENDER_WIDTH`, `RENDER_HEIGHT`, and two `static_assert`s.
- Comment at top updated to reference v2a and the new spec.

- [ ] **Step 2: (No compile check here)**

The project compiles only after Task 2 is in. Proceed directly.

- [ ] **Step 3: Commit**

```bash
git add config.h
git commit -m "refactor(config): split render dims from source dims; drop EYE_RENDER_*"
```

---

## Task 2: Rewrite `drawEye()` — render-space loop + Bresenham source mapping

**Rationale:** Core of v2a. Replace the 240×240 inner-loop with a 466×466 render-space loop that reads source-space indices via one Y Bresenham accumulator and three X Bresenham accumulators (`scleraX`, `irisX`, `lidX`). All pixel-decision logic (eyelid threshold → iris polar → sclera fallback) is preserved bit-for-bit.

The entire file is pasted below so the engineer does not have to splice changes across the existing `drawEye()` and risk leaving stale code in place. Only `drawEye()` changes; `initEyes()`, `updateEye()`, `frame()`, `split()`, and the `ease[]` / `oldIris` / `newIris` / `timeOfLastBlink` declarations are identical to v1.

**Files:**
- Modify (overwrite): `eye_functions.ino`

- [ ] **Step 1: Overwrite `eye_functions.ino`**

```cpp
// Scanline-streaming renderer for the Uncanny Eyes.
// v2a: renders in render-space (466x466) and reads a 240-baked source
// asset via Bresenham integer accumulators (nearest-neighbor upscale).
// See docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md.
//
// Originally written by Phil Burgess / Paint Your Dragon for Adafruit
// Industries (MIT license). Adapted to this project's single-eye Waveshare
// ESP32-S3 AMOLED target.

// Autonomous iris motion uses a fractal behavior to simulate both the
// major reaction of the eye plus the continuous smaller adjustments.
uint16_t oldIris = (IRIS_MIN + IRIS_MAX) / 2, newIris;

// Initialise eye ----------------------------------------------------------
void initEyes(void) {
  Serial.println("initEyes: single eye v2a");
  eye.blink.state = NOBLINK;
}

// UPDATE EYE --------------------------------------------------------------
void updateEye(void) {
  newIris = random(IRIS_MIN, IRIS_MAX);
  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}

// EYE-RENDERING FUNCTION --------------------------------------------------
// Inputs are in source-space. scleraX / scleraY point into the SCLERA_WIDTH
// x SCLERA_HEIGHT sclera array; iris offsets are computed internally;
// thresholds uT / lT gate the upper / lower eyelid masks.
void drawEye(
    uint32_t iScale,   // Scale factor for iris
    uint32_t scleraX,  // First pixel X offset into sclera image (source-space)
    uint32_t scleraY,  // First pixel Y offset into sclera image (source-space)
    uint32_t uT,       // Upper eyelid threshold value
    uint32_t lT) {     // Lower eyelid threshold value
  uint32_t p, a, d;
  uint32_t pixels = 0;

  display_startWrite();
  display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

  // Left-eye walks the eyelid map right-to-left (caruncle on nose-side);
  // right-eye walks it left-to-right. Both directions work with the same
  // Bresenham cadence, just with step sign flipped.
  const int32_t lidX_start = (EYE_SIDE == EYE_SIDE_LEFT) ? (SCREEN_WIDTH - 1) : 0;
  const int32_t lidX_step  = (EYE_SIDE == EYE_SIDE_LEFT) ? -1 : 1;

  const uint32_t scleraXsave = scleraX;
  int32_t irisX, irisY;
  irisY = (int32_t)scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  // Single Y accumulator -- screenY, scleraY, irisY all tick together.
  uint16_t screenY  = 0;
  uint16_t y_accum  = 0;

  for (uint16_t ry = 0; ry < RENDER_HEIGHT; ry++) {
    // Per-render-row X trackers: reset at the start of each row.
    int32_t  scleraX_src = (int32_t)scleraXsave;
    int32_t  irisX_src   = (int32_t)scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    int32_t  lidX_src    = lidX_start;
    uint16_t x_accum     = 0;

    for (uint16_t rx = 0; rx < RENDER_WIDTH; rx++) {
      // Pixel resolution -- identical to v1, just using the per-pixel
      // source-space trackers (screenY, scleraY, irisY, scleraX_src,
      // irisX_src, lidX_src) instead of v1's tight screenX/screenY
      // increments.
      if ((pgm_read_byte(lower + screenY * SCREEN_WIDTH + lidX_src) <= lT) ||
          (pgm_read_byte(upper + screenY * SCREEN_WIDTH + lidX_src) <= uT)) {
        // Covered by eyelid
        p = 0;
      } else if ((irisY < 0) || (irisY >= IRIS_HEIGHT) ||
                 (irisX_src < 0) || (irisX_src >= IRIS_WIDTH)) {
        // In sclera
        p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX_src);
      } else {
        // Maybe iris...
        p = pgm_read_word(polar + irisY * IRIS_WIDTH + irisX_src);
        d = (iScale * (p & 0x7F)) / 128;
        if (d < IRIS_MAP_HEIGHT) {
          a = (IRIS_MAP_WIDTH * (p >> 7)) / 512;
          p = pgm_read_word(iris + d * IRIS_MAP_WIDTH + a);
        } else {
          p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX_src);
        }
      }

      // Arduino_GFX's writePixels() takes host-order RGB565 and swaps
      // to bus byte order internally -- do NOT pre-swap here.
      pbuffer[dmaBuf][pixels++] = p;

      if (pixels >= BUFFER_SIZE) {
        yield();
        display_writePixels(&pbuffer[dmaBuf][0], pixels);
        dmaBuf = !dmaBuf;
        pixels = 0;
      }

      // Advance X source trackers via Bresenham.
      // All three X indices share the same SCREEN_WIDTH / RENDER_WIDTH
      // cadence (they differ only in start value and step sign).
      x_accum += SCREEN_WIDTH;
      while (x_accum >= RENDER_WIDTH) {
        x_accum -= RENDER_WIDTH;
        scleraX_src += 1;
        irisX_src   += 1;
        lidX_src    += lidX_step;
      }
    }

    // Advance Y source trackers via Bresenham (screenY, scleraY, irisY).
    y_accum += SCREEN_HEIGHT;
    while (y_accum >= RENDER_HEIGHT) {
      y_accum -= RENDER_HEIGHT;
      screenY += 1;
      scleraY += 1;
      irisY   += 1;
    }
  }

  if (pixels) {
    display_writePixels(&pbuffer[dmaBuf][0], pixels);
  }
  display_endWrite();
}

// EYE ANIMATION -----------------------------------------------------------

const uint8_t ease[] = { // Ease in/out curve for eye movements 3*t^2-2*t^3
  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,
  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  7,  8,  9,  9, 10, 10,
  11, 12, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23,
  24, 25, 26, 27, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 57, 58,
  60, 61, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80,
  81, 83, 84, 85, 87, 88, 90, 91, 93, 94, 96, 97, 98, 100, 101, 103,
  104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 119, 121, 122, 124, 125, 127,
  128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143, 145, 146, 148, 149, 151,
  152, 154, 155, 157, 158, 159, 161, 162, 164, 165, 167, 168, 170, 171, 172, 174,
  175, 177, 178, 179, 181, 182, 183, 185, 186, 188, 189, 190, 192, 193, 194, 195,
  197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210, 211, 213, 214, 215,
  216, 217, 218, 219, 220, 221, 222, 224, 225, 226, 227, 228, 228, 229, 230, 231,
  232, 233, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244,
  245, 245, 246, 246, 247, 248, 248, 249, 249, 250, 250, 251, 251, 251, 252, 252,
  252, 253, 253, 253, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255
};

#ifdef AUTOBLINK
uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L;
#endif

// Process motion for a single frame. Takes the iris scale value and drives
// the full animation -> render pipeline. All motion math stays in
// source-space; render-space only enters drawEye().
void frame(uint16_t iScale) {
  static uint32_t frames = 0;
  int16_t         eyeX, eyeY;
  uint32_t        t = micros();

  if (!(++frames & 255)) {
    float elapsed = (millis() - startTime) / 1000.0f;
    if (elapsed > 0) {
      Serial.print("FPS=");
      Serial.println((uint16_t)(frames / elapsed));
    }
  }

  // --- Autonomous X/Y eye motion ---
  static bool     eyeInMotion      = false;
  static int16_t  eyeOldX = 512, eyeOldY = 512, eyeNewX = 512, eyeNewY = 512;
  static uint32_t eyeMoveStartTime = 0L;
  static int32_t  eyeMoveDuration  = 0L;

  int32_t dt = t - eyeMoveStartTime;
  if (eyeInMotion) {
    if (dt >= eyeMoveDuration) {
      eyeInMotion      = false;
      eyeMoveDuration  = random(3000000);
      eyeMoveStartTime = t;
      eyeX = eyeOldX = eyeNewX;
      eyeY = eyeOldY = eyeNewY;
    } else {
      int16_t eased = ease[255 * dt / eyeMoveDuration] + 1;
      eyeX = eyeOldX + (((eyeNewX - eyeOldX) * eased) / 256);
      eyeY = eyeOldY + (((eyeNewY - eyeOldY) * eased) / 256);
    }
  } else {
    eyeX = eyeOldX;
    eyeY = eyeOldY;
    if (dt > eyeMoveDuration) {
      int16_t  dx, dy;
      uint32_t d2;
      do {
        eyeNewX = random(1024);
        eyeNewY = random(1024);
        dx      = (eyeNewX * 2) - 1023;
        dy      = (eyeNewY * 2) - 1023;
      } while ((d2 = (dx * dx + dy * dy)) > (1023 * 1023));
      eyeMoveDuration  = random(72000, 144000);
      eyeMoveStartTime = t;
      eyeInMotion      = true;
    }
  }

  // --- Autonomous blinking ---
#ifdef AUTOBLINK
  if ((t - timeOfLastBlink) >= timeToNextBlink) {
    timeOfLastBlink = t;
    uint32_t blinkDuration = random(36000, 72000);
    if (eye.blink.state == NOBLINK) {
      eye.blink.state     = ENBLINK;
      eye.blink.startTime = t;
      eye.blink.duration  = blinkDuration;
    }
    timeToNextBlink = blinkDuration * 3 + random(4000000);
  }
#endif

  if (eye.blink.state) {
    if ((t - eye.blink.startTime) >= eye.blink.duration) {
      if (++eye.blink.state > DEBLINK) {
        eye.blink.state = NOBLINK;
      } else {
        eye.blink.duration *= 2;
        eye.blink.startTime = t;
      }
    }
  }

  // --- Map to pixel units (source-space) ---
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH  - SCREEN_WIDTH);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - SCREEN_HEIGHT);

  // Slight convergence (source-space) so a pair of eyes looks fixated.
  eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4;
  if (eyeX > (SCLERA_WIDTH - SCREEN_WIDTH)) eyeX = SCLERA_WIDTH - SCREEN_WIDTH;
  if (eyeX < 0) eyeX = 0;

  // --- Eyelid tracking (source-space) ---
  static uint8_t uThreshold = 128;
  uint8_t        lThreshold, n;
#ifdef TRACKING
  int16_t sampleX = SCLERA_WIDTH  / 2 - (eyeX / 2);
  int16_t sampleY = SCLERA_HEIGHT / 2 - (eyeY + IRIS_HEIGHT / 4);
  if (sampleY < 0) {
    n = 0;
  } else {
    n = (pgm_read_byte(upper + sampleY * SCREEN_WIDTH + sampleX) +
         pgm_read_byte(upper + sampleY * SCREEN_WIDTH + (SCREEN_WIDTH - 1 - sampleX))) / 2;
  }
  uThreshold = (uThreshold * 3 + n) / 4;
  lThreshold = 254 - uThreshold;
#else
  uThreshold = lThreshold = 0;
#endif

  if (eye.blink.state) {
    uint32_t s = (t - eye.blink.startTime);
    if (s >= eye.blink.duration) {
      s = 255;
    } else {
      s = 255 * s / eye.blink.duration;
    }
    s = (eye.blink.state == DEBLINK) ? 1 + s : 256 - s;
    n          = (uThreshold * s + 254 * (257 - s)) / 256;
    lThreshold = (lThreshold * s + 254 * (257 - s)) / 256;
  } else {
    n = uThreshold;
  }

  drawEye(iScale, eyeX, eyeY, n, lThreshold);
}

// AUTONOMOUS IRIS SCALING -------------------------------------------------

void split(
    int16_t  startValue,
    int16_t  endValue,
    uint32_t startTime_local,
    int32_t  duration,
    int16_t  range) {
  if (range >= 8) {
    range    /= 2;
    duration /= 2;
    int16_t  midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime  = startTime_local + duration;
    split(startValue, midValue, startTime_local, duration, range);
    split(midValue,   endValue, midTime,         duration, range);
  } else {
    int32_t dt;
    int16_t v;
    while ((dt = (micros() - startTime_local)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if (v < IRIS_MIN)      v = IRIS_MIN;
      else if (v > IRIS_MAX) v = IRIS_MAX;
      frame(v);
    }
  }
}
```

- [ ] **Step 2: Verify it compiles**

Arduino IDE → **Verify** (or `arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default .`).

Expected:
- Compile succeeds.
- No warnings referencing `EYE_RENDER_X`, `EYE_RENDER_Y`, `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`.
- No new warnings that weren't present in v1 (specifically, no signed/unsigned comparison warnings introduced by the `int32_t` source-index trackers meeting `uint32_t` iris bounds).

If the compile fails with `SCREEN_WIDTH` or `SCREEN_HEIGHT` undefined, confirm `config.h` still `#include "data/default_large.h"` at the top. That include must stay — it is what defines the source-space constants.

- [ ] **Step 3: Commit**

```bash
git add eye_functions.ino
git commit -m "feat(renderer): full-panel 466x466 NN via Bresenham source mapping

drawEye() now iterates in render-space (RENDER_WIDTH x RENDER_HEIGHT)
and reads source-space via one Y accumulator and three X accumulators
(scleraX, irisX, lidX). Collapses to identity when source == render,
so a future native-466 asset swaps in with no renderer change. Pixel
decision logic (eyelid / iris polar / sclera fallback) unchanged."
```

---

## Task 3: Flash, verify the eye fills the panel, measure FPS

**Rationale:** End-to-end acceptance gate. Every success criterion in the spec is checked here. Troubleshooting guidance is bounded — if we miss FPS, we do not re-open the design; we either accept and file a v2a-perf follow-up or apply the listed mitigations.

**Files:** none modified. This task produces evidence (serial-log snippet) that v2a works.

- [ ] **Step 1: Upload to the board**

Connect Waveshare ESP32-S3-Touch-AMOLED-1.75 via USB-C. Arduino IDE → **Upload** (or `arduino-cli upload --fqbn <FQBN> -p /dev/tty.usbmodem* .`).

- [ ] **Step 2: Open serial monitor at 115200 baud**

Expected serial output within ~2 s of reset:

```
uncanny-eyes: boot
initEyes: single eye v2a
uncanny-eyes: display_begin()
uncanny-eyes: running
FPS=<n>
FPS=<n>
...
```

Every 256 frames a new `FPS=<n>` line is logged.

- [ ] **Step 3: Visually verify the render**

Look at the round AMOLED. Expected:
- The eye fills the **full** 466×466 panel — no centered inset, no black border beyond the panel's own round edge mask.
- The eye moves autonomously to new gaze points with smooth easing.
- The iris scales smoothly over several seconds.
- The upper eyelid tracks the pupil (lid closes more when the eye looks down).
- The eye blinks at random intervals.
- No flicker, no visible tearing, no freeze, no garbage strip at any edge.

- [ ] **Step 4: Record measured FPS**

Watch at least three `FPS=<n>` lines. Take the median.

Acceptance:
- **FPS ≥ 10** → v2a done, proceed.
- **FPS ≥ 20** → stretch goal met, note in the commit message.
- **FPS < 10** → do **not** mark v2a done. Apply Step 5 mitigations in order; if still < 10 after all three, file a v2a-perf follow-up spec rather than expanding this one.

- [ ] **Step 5: Troubleshoot (only if Step 3 or 4 fails)**

| Symptom                                        | First thing to try                                                                                         |
| ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| Panel stays black                              | v1 already proved the display bring-up works — suspect a recent commit regression. `git diff main` on `display.ino`; it should be unchanged.   |
| 6-px garbage strip at left edge                | `CO5300_COL_OFFSET1 = 6` in `display.ino` is being ignored by Arduino_GFX at the new full-width address window. Verify the value is still there; add a `Serial.printf` in `display_begin` to confirm it's applied. |
| Eye mirrored left/right                        | `EYE_SIDE` wrong for this board. Flip the `#define` in `config.h`.                                         |
| Eye appears correct but is 2× smaller than panel (still inset) | Confirm `display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT)` was applied. Grep for `EYE_RENDER_` — any surviving reference is a merge mistake. |
| Colors inverted                                | This shouldn't happen — v1 already settled on "no pre-swap". Re-check you didn't add `p >> 8 \| p << 8` back in.                |
| FPS < 10                                       | (a) Confirm build used `-O2` (Arduino IDE default at 240 MHz is `-Os`; switch to `-O2` via platform.txt override or just accept current build). (b) Cache row base pointers once per row: declare `const uint8_t *upperRow = upper + screenY * SCREEN_WIDTH;` at the top of each render row, index with `upperRow[lidX_src]`. Same for `lower` and `sclera`. (c) If still short, file v2a-perf follow-up. |
| FPS visibly steppier motion (chunky animation) | Not a defect — NN-stretched 240→466 has coarser source resolution than render resolution. Covered in the spec's risk list.   |

Do **not** change the Bresenham cadence, the pixel-decision logic, or the pixel buffer size in response to FPS alone. Those are design decisions.

- [ ] **Step 6: Flash the right-eye build, verify mirror**

Temporarily change `config.h`:

```cpp
#define EYE_SIDE EYE_SIDE_RIGHT
```

Upload, look at the panel, confirm:
- Eye is visible and correctly rendered (not just a black screen or mirrored garbage).
- Eyelid shape looks mirrored compared to the left-eye build (caruncle / tear duct on the opposite side).

Then revert the `#define` back to `EYE_SIDE_LEFT` and do **not** commit the right-eye change — it's just a verification flash.

```bash
git diff config.h    # expected: no changes
```

- [ ] **Step 7: Capture evidence and commit**

Include the 3+ FPS lines and which `EYE_SIDE` values rendered correctly in the commit message:

```bash
git commit --allow-empty -m "test(v2a): hardware verification -- full-panel render at <N> FPS

EYE_SIDE_LEFT:  correct orientation, no garbage strip
EYE_SIDE_RIGHT: correct mirror, no garbage strip

Serial log (left-eye build):
FPS=<n1>
FPS=<n2>
FPS=<n3>"
```

---

## Task 4: Update `README.md` for full-panel render

**Rationale:** The existing README describes v1 behavior ("240×240 centered on the round panel"). A reader arriving fresh should know the current firmware fills the whole panel. Targeted edit, not a rewrite — most of the README is still accurate.

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace the v1 behavior paragraph**

Find this paragraph in `README.md`:

```
v1 status: one board renders a single eye (left), autonomously moving, blinking,
and tracking eyelids. The eye is rendered at its native 240×240 resolution,
centered on the round panel.
```

Replace with:

```
Status: one board renders a single eye (left), autonomously moving, blinking,
and tracking eyelids. The eye fills the full 466×466 panel, nearest-neighbor
stretched at runtime from the 240-baked `default_large` asset via a
size-agnostic scanline renderer. See
[`docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md`](docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md)
for the design.
```

- [ ] **Step 2: Verify no other "240×240 centered" references remain**

```bash
grep -n "240.*centered\|centered.*240\|EYE_RENDER" README.md
```

Expected: no matches.

If any matches appear, update the surrounding prose to match the full-panel render. None should remain.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs(readme): describe full-panel v2a render"
```

---

## Task 5: Size-agnostic smoke test (optional, recommended once)

**Rationale:** Spec Goal #4 promises the renderer handles any source asset size without touching `drawEye()`. We can prove this cheaply by compiling (but **not flashing**) against a synthetic 120-scaled stub header. This is a one-time test — we discard it afterwards.

**Files:**
- Create (temporary): `data/_smoke_120.h`
- Modify (temporary): `config.h`

- [ ] **Step 1: Create a 120-scaled stub asset header**

This is not a visually meaningful eye — it's a compile-smoke stub with valid dimensions and 1-element arrays, just enough to prove the renderer compiles with different source sizes. The renderer reads past these arrays at runtime, so **do not flash this build**.

Create `data/_smoke_120.h`:

```cpp
#pragma once

// Synthetic 120-scaled stub for the size-agnostic compile smoke test.
// NOT A REAL ASSET -- do not flash a build that uses this header.

#define SCLERA_WIDTH  188
#define SCLERA_HEIGHT 188
const uint16_t sclera[1] PROGMEM = { 0x0000 };

#define IRIS_MAP_WIDTH  245
#define IRIS_MAP_HEIGHT 245
const uint16_t iris[1] PROGMEM = { 0x0000 };

#define SCREEN_WIDTH  120
#define SCREEN_HEIGHT 120
const uint8_t upper[1] PROGMEM = { 0 };
const uint8_t lower[1] PROGMEM = { 0 };

#define IRIS_WIDTH  90
#define IRIS_HEIGHT 90
const uint16_t polar[1] PROGMEM = { 0x0000 };

#define IRIS_MIN 90
#define IRIS_MAX 130
```

- [ ] **Step 2: Temporarily repoint `config.h`**

In `config.h`, change:

```cpp
#include "data/default_large.h"
```

to:

```cpp
#include "data/_smoke_120.h"
```

- [ ] **Step 3: Compile only (do not upload)**

Arduino IDE → **Verify** (or `arduino-cli compile ...`).

Expected: compile succeeds with no errors. The two `static_assert`s in `config.h` pass (`SCREEN_WIDTH = 120 <= RENDER_WIDTH = 466`; same for height). No `drawEye()` changes required — this is the size-agnostic promise in action.

If compile fails due to a `drawEye()` symbol or size assumption, the renderer is not actually size-agnostic. Fix `drawEye()`; the spec requirement is what's binding, not this temporary header.

- [ ] **Step 4: Revert and delete the stub**

```bash
# Revert config.h
git checkout -- config.h
# Delete the stub (it was never committed)
rm data/_smoke_120.h
```

- [ ] **Step 5: Confirm clean working tree**

```bash
git status
```

Expected: nothing to commit, working tree clean.

(Nothing to commit in this task — the smoke test is a compile-only check, not a code change.)

---

## Self-Review

**1. Spec coverage:**

| Spec section                                              | Covered by                |
| --------------------------------------------------------- | ------------------------- |
| Goal 1: full-panel 466×466 render                         | Task 2, Task 3 Step 3     |
| Goal 2: animation behaves same as v1                      | Task 2 (frame/split unchanged), Task 3 Step 3 |
| Goal 3: FPS ≥ 10 sustained                                | Task 3 Step 4             |
| Goal 4: no assumption about source asset dims             | Task 2 (Bresenham loop), Task 5 (compile proof) |
| Goal 5: left + right-eye builds both render               | Task 3 Step 6             |
| Non-goal: no bilinear, no new assets, no toggle           | Task 2 (only NN Bresenham, `EYE_RENDER_*` deleted) |
| Architecture: source-space / render-space separation      | Task 1 (config), Task 2 (renderer) |
| Architecture: `drawEye()` + `setAddrWindow` are the only render-space sites | Task 2 (all other functions untouched) |
| Config change: drop EYE_RENDER_*, add RENDER_*            | Task 1                    |
| File change: README targeted edit                         | Task 4                    |
| Risk: FPS under floor                                     | Task 3 Step 5 row "FPS < 10" |
| Risk: Bresenham off-by-one                                | Task 1 (`static_assert`s), Task 2 (accumulator-after-use form) |
| Risk: left-eye mirror Bresenham                           | Task 3 Step 6 (right-eye flash) |
| Risk: visible NN stepping                                 | Task 3 Step 5 row "FPS visibly steppier motion" (documented, accepted) |
| Risk: CO5300 col-offset at full width                     | Task 3 Step 5 row "6-px garbage strip" |
| Success criterion: no surviving `EYE_RENDER_*` reference  | Task 1 (deleted), Task 4 Step 2 (grep README) |
| Success criterion: size-agnostic smoke                    | Task 5                    |

No spec section is uncovered.

**2. Placeholder scan:** No "TBD" / "TODO" / "implement later" / "add appropriate error handling". Every code block is complete. The one `<N>` and `<n1>`/`<n2>`/`<n3>` in Task 3 Step 7 commit message are intentional — they're values measured at flash time, not predictable ahead.

**3. Type / name consistency:**

- `RENDER_WIDTH`, `RENDER_HEIGHT`, `PANEL_WIDTH`, `PANEL_HEIGHT` — defined in Task 1, used in Task 2's `display_setAddrWindow` call and the Bresenham denominators.
- `SCREEN_WIDTH`, `SCREEN_HEIGHT` — from `data/default_large.h`, referenced in Task 1's `static_assert`s and in Task 2's Bresenham numerators and pixel-index arithmetic. Task 5's stub defines both at 120.
- `SCLERA_WIDTH`, `SCLERA_HEIGHT`, `IRIS_WIDTH`, `IRIS_HEIGHT`, `IRIS_MAP_WIDTH`, `IRIS_MAP_HEIGHT` — all from the asset header, used unchanged in Task 2.
- `EYE_SIDE`, `EYE_SIDE_LEFT`, `EYE_SIDE_RIGHT` — Task 1 (define), Task 2 (lid step + convergence), Task 3 Step 6 (mirror verify).
- `scleraX_src`, `irisX_src`, `lidX_src`, `screenY`, `x_accum`, `y_accum` — all introduced in Task 2 only, consistent within the file.
- `lidX_start`, `lidX_step` — renamed `uint16_t` → `int32_t` for signed-decrement safety on the left-eye path. Used consistently within Task 2.
- `pbuffer`, `dmaBuf`, `BUFFER_SIZE` — defined in `ESP32-uncanny-eyes-halloween-skull.ino` (unchanged), used in Task 2's `drawEye()`.
- `eye.blink.*`, `NOBLINK`/`ENBLINK`/`DEBLINK` — same definitions as v1, used unchanged in Task 2's `frame()`.

All consistent.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-18-v2a-full-panel-render.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
