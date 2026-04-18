# ESP32-S3 AMOLED Port (v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the Uncanny Eyes sketch to one Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466×466) so it renders the `default_large` left eye as a 240×240 image centered on the round panel, with autonomous motion / blink / iris / eyelid tracking.

**Architecture:** Keep the existing scanline-streaming renderer. Swap `TFT_eSPI` for `Arduino_GFX` behind a thin wrapper (`display.ino`). Collapse all 2-eye / jaw / joystick / light-sensor / wink-button code paths. Drive left/right eye asymmetry from a single `EYE_SIDE` compile-time constant.

**Tech Stack:** Arduino IDE 2.x, ESP32 core for Arduino (≥ 3.0), `Arduino_GFX` library (moononournation), `default_large.h` eye asset (unchanged).

**Testing model:** This is a microcontroller sketch. "Tests" are: (a) `arduino-cli compile` or Arduino IDE Verify produces no errors/warnings, and (b) flashing to the Waveshare board produces the expected visual and serial output. Each task ends with a compile check and, where applicable, a flash-and-look step.

**Reference spec:** `docs/superpowers/specs/2026-04-18-esp32-s3-amoled-port-design.md`

---

## File Structure After Implementation

```
uncanny-eyes-skull.ino     main sketch — setup() / loop()
config.h                   compile-time config (EYE_SIDE, brightness, render origin)
eye_functions.ino          eye state machine + renderer (drawEye / frame / split)
display.ino                Arduino_GFX wrapper: display_begin / _setAddrWindow /
                           _writePixels / _fillScreen / _setBrightness / _startWrite /
                           _endWrite
data/default_large.h       baked eye graphics (unchanged, only asset kept)
docs/
  hardware-notes.md        pin map + peripheral init notes (created by Task 1)
  superpowers/
    specs/2026-04-18-esp32-s3-amoled-port-design.md
    plans/2026-04-18-esp32-s3-amoled-port.md           (this file)
tools/
  hello_amoled/
    hello_amoled.ino       throwaway bring-up spike (created by Task 2)
README.md                  rewritten for new hardware
LICENSE                    unchanged
```

**Deleted** during implementation: `user_empty.cpp`, `user_skull-jaw.cpp`, `wiring/`, `data/{catEye,defaultEye,doeEye,dragonEye,goatEye,logo,naugaEye,newtEye,noScleraEye,owlEye,terminatorEye}.h`.

---

## Arduino IDE board settings (use for every compile/upload in this plan)

All tasks that compile or flash assume the following Arduino IDE **Tools** menu settings. Verify once at the start of Task 2 and don't change them afterward.

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

Equivalent `arduino-cli` FQBN (optional, for scripted builds):
`esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default`

---

## Task 1: Record Waveshare board pin map and peripheral-init facts

**Rationale:** Before any code change, pin the facts down. The CO5300 QSPI pins, the I²C bus (for TCA9554 / AXP2101), the display reset routing, and any required AXP2101 register writes are all board-specific. Copying these verbatim from the Waveshare demo is the single biggest de-risking move in the whole plan — guessing any of them loses hours.

**Files:**
- Create: `docs/hardware-notes.md`

**How to source the facts (any one of these works):**
1. **Waveshare wiki page** for the product: `www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75` — look for a schematic PDF and/or sample Arduino project ZIP.
2. **Waveshare GitHub** `ESP32-S3-Touch-AMOLED-1.75` or `ESP32-S3-Touch-AMOLED-1.75-Demo` repository.
3. **Arduino_GFX dev-device-list** on moononournation/Arduino_GFX — check if this exact board is listed as a supported device with a ready-made init snippet.

- [ ] **Step 1: Fetch the Waveshare wiki / demo source and schematic**

Download one of:
- `ESP32-S3-Touch-AMOLED-1.75 Arduino Demo.zip` from the wiki's "Resources" section, **or**
- Clone `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75` (name may vary), **or**
- Open the schematic PDF linked on the wiki.

- [ ] **Step 2: Extract the pin map**

From the demo's `User_Setup.h` / `Arduino_GFX_Library.h` include block / schematic, record the following values in `docs/hardware-notes.md`:

```markdown
# Waveshare ESP32-S3-Touch-AMOLED-1.75 — hardware notes

Source: <URL or filename of demo/schematic used>
Captured: 2026-04-18

## CO5300 AMOLED (QSPI)

| Signal | ESP32-S3 GPIO |
| ------ | ------------- |
| QSPI CS    | GPIO??        |
| QSPI SCLK  | GPIO??        |
| QSPI D0    | GPIO??        |
| QSPI D1    | GPIO??        |
| QSPI D2    | GPIO??        |
| QSPI D3    | GPIO??        |
| TE         | GPIO?? (or -1 if not wired) |
| RESET      | behind TCA9554 bit ?? / direct GPIO?? |

QSPI clock used by demo: <value in MHz>

## I²C bus

| Signal | ESP32-S3 GPIO |
| ------ | ------------- |
| SDA    | GPIO??        |
| SCL    | GPIO??        |

## TCA9554 GPIO expander

- I²C address: 0x??
- Pin used as CO5300 RESX: P?
- Any other pins this design touches (e.g. touch RST): P?

## AXP2101 PMU

- I²C address: 0x34
- Register writes performed by the Waveshare demo before display init:
  - `REG 0x??` ← `0x??`  // enables <rail name> at <voltage>
  - …

## CO5300 init quirks

- Row offset on this panel: <0 or specific value>
- Brightness command: `0x51` (display brightness), range 0..255
- Column start / row start values passed to setAddrWindow: <values>
```

Fill in every `??` with the real value from the demo. If a field doesn't apply (e.g. no row offset), write `n/a` and say why.

- [ ] **Step 3: Commit**

```bash
git add docs/hardware-notes.md
git commit -m "docs: capture Waveshare ESP32-S3-Touch-AMOLED-1.75 pin map and init"
```

---

## Task 2: Bring-up spike — prove the display, library, and pin map

**Rationale:** This is a pure hardware/library smoke test. If `Arduino_GFX` + our recorded pins can't light up the panel with a solid color, we must not proceed to porting the renderer — we'd be debugging two things at once. A throwaway sketch isolates the risk. Stays in the repo (under `tools/`) so anyone can re-run the smoke test later.

**Files:**
- Create: `tools/hello_amoled/hello_amoled.ino`

- [ ] **Step 1: Install the `Arduino_GFX` library**

In Arduino IDE 2.x: *Library Manager* → search for `GFX Library for Arduino` by `moononournation` → install the latest version (≥ 1.4.x). This adds `Arduino_GFX_Library.h` and friends.

- [ ] **Step 2: Write the spike sketch**

Create `tools/hello_amoled/hello_amoled.ino`. Substitute the real pin values recorded in `docs/hardware-notes.md` for every `PIN_*` macro below. Then copy in (or re-create) any AXP2101 and TCA9554 init steps the Waveshare demo performs before `gfx->begin()`.

```cpp
// Smoke test: fill the 466x466 CO5300 AMOLED with a solid color and
// cycle red->green->blue every 2 s. Verifies Arduino_GFX + pin map +
// board power sequencing before we port the real project.

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// Pin values — fill from docs/hardware-notes.md:
#define PIN_QSPI_CS    XX
#define PIN_QSPI_SCLK  XX
#define PIN_QSPI_D0    XX
#define PIN_QSPI_D1    XX
#define PIN_QSPI_D2    XX
#define PIN_QSPI_D3    XX
#define PIN_I2C_SDA    XX
#define PIN_I2C_SCL    XX

#define PANEL_W  466
#define PANEL_H  466

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    PIN_QSPI_CS, PIN_QSPI_SCLK,
    PIN_QSPI_D0, PIN_QSPI_D1, PIN_QSPI_D2, PIN_QSPI_D3);

// Arduino_GFX constructor: last arg is rotation, then ips, col_offset, row_offset.
// Use the row/col offsets recorded in hardware-notes.md (often 0 or 6).
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, GFX_NOT_DEFINED /* RST pin handled externally */,
    0 /* rotation */, false /* ips */,
    PANEL_W, PANEL_H,
    0 /* col_offset1 */, 0 /* row_offset1 */,
    0 /* col_offset2 */, 0 /* row_offset2 */);

static void power_on_display_rail() {
  // Replace with AXP2101 register writes from docs/hardware-notes.md.
  // Example shape (values must come from the demo, do not guess):
  //   Wire.beginTransmission(0x34);
  //   Wire.write(0x90); Wire.write(0xBF);
  //   Wire.endTransmission();
}

static void pulse_display_reset() {
  // Replace with TCA9554 writes to toggle RESX low -> high, from
  // docs/hardware-notes.md. If RESX is a direct GPIO, do pinMode/digitalWrite
  // here instead and pass that pin to Arduino_CO5300.
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("hello_amoled: starting");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  power_on_display_rail();
  pulse_display_reset();

  if (!gfx->begin(40000000 /* 40 MHz QSPI, bump later if stable */)) {
    Serial.println("hello_amoled: gfx->begin() FAILED");
    while (true) { delay(1000); }
  }
  gfx->fillScreen(0x0000);
  gfx->Display_Brightness(200);
  Serial.println("hello_amoled: display up");
}

void loop() {
  static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F }; // R, G, B in RGB565
  static uint8_t i = 0;
  gfx->fillScreen(colors[i]);
  Serial.printf("hello_amoled: color %u\n", i);
  i = (i + 1) % 3;
  delay(2000);
}
```

- [ ] **Step 3: Verify it compiles**

With the board settings from the header, run Arduino IDE's **Verify** button (or `arduino-cli compile --fqbn <FQBN> tools/hello_amoled`).

Expected: compile succeeds, no warnings related to missing symbols. If it fails because `Arduino_CO5300`'s constructor signature differs in the installed library version, adjust the argument list to match (the library header is the source of truth) — this is not a spec change.

- [ ] **Step 4: Flash and verify on hardware**

Connect the board via USB-C, select the correct port, **Upload**. Open the Serial Monitor at 115200 baud.

Expected:
- Serial prints: `hello_amoled: starting` → `hello_amoled: display up` → then `hello_amoled: color 0/1/2` every 2 s.
- The round panel shows a solid red, then green, then blue, looping.

If the panel stays black or white: revisit `power_on_display_rail()` and `pulse_display_reset()`. Do **not** proceed until colors display correctly.

- [ ] **Step 5: Commit**

```bash
git add tools/hello_amoled/hello_amoled.ino
git commit -m "feat(tools): add hello_amoled bring-up spike for CO5300 QSPI"
```

---

## Task 3: Delete dead code and dead assets

**Rationale:** Old project files reference hardware we no longer target (`TFT_eSPI`, `GC9A01`, two-eye layout, jaw servo). Deleting them now — before the rewrite — prevents the temptation to "just keep them around in case." After this task the project will not compile; the next three tasks restore compilability.

**Files to delete:**
- `user_empty.cpp`
- `user_skull-jaw.cpp`
- `wiring/` (whole folder)
- `data/catEye.h`, `data/defaultEye.h`, `data/doeEye.h`, `data/dragonEye.h`, `data/goatEye.h`, `data/logo.h`, `data/naugaEye.h`, `data/newtEye.h`, `data/noScleraEye.h`, `data/owlEye.h`, `data/terminatorEye.h`

- [ ] **Step 1: Remove the files**

```bash
git rm user_empty.cpp user_skull-jaw.cpp
git rm -r wiring
git rm data/catEye.h data/defaultEye.h data/doeEye.h data/dragonEye.h \
       data/goatEye.h data/logo.h data/naugaEye.h data/newtEye.h \
       data/noScleraEye.h data/owlEye.h data/terminatorEye.h
```

- [ ] **Step 2: Verify what's left**

```bash
ls data/
```

Expected output: only `default_large.h`.

```bash
ls
```

Expected: no `user_*.cpp`, no `wiring/`.

- [ ] **Step 3: Commit**

```bash
git commit -m "chore: delete dead code paths (old hardware, jaw servo, unused eye assets)"
```

---

## Task 4: Rewrite `config.h` to its minimal v1 form

**Rationale:** `config.h` is currently overloaded with config for 2 eyes, backlight, joystick, light sensor, wink buttons. Strip it to just: eye side, panel geometry, render-window geometry, brightness, behavior flags.

**Files:**
- Modify (overwrite): `config.h`

- [ ] **Step 1: Overwrite `config.h` with the v1 version**

```cpp
// v1 config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466)
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

// Iris range defaults — can be overridden by the eye asset header.
#if !defined(IRIS_MIN)
  #define IRIS_MIN  90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX  130
#endif
```

- [ ] **Step 2: Commit**

```bash
git add config.h
git commit -m "refactor(config): collapse to v1 single-eye AMOLED config"
```

---

## Task 5: Create the `display.ino` wrapper around `Arduino_GFX`

**Rationale:** Keep everything `Arduino_GFX`-specific in one file. The renderer (`eye_functions.ino`) calls into this module through a small, `TFT_eSPI`-shaped API — so the renderer port in Task 6 is mostly mechanical renaming.

**Files:**
- Create: `display.ino`

- [ ] **Step 1: Write the wrapper**

Substitute the `PIN_*` values from `docs/hardware-notes.md`, and the AXP2101 / TCA9554 init sequences from the spike in Task 2. Do not re-derive anything; copy the exact values that worked in `hello_amoled`.

```cpp
// display.ino — thin wrapper around Arduino_GFX for the Waveshare
// ESP32-S3-Touch-AMOLED-1.75 (CO5300, QSPI, 466x466).
// All Arduino_GFX knowledge lives in this file.

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// Pin values — copy from docs/hardware-notes.md (the same values the
// hello_amoled spike used).
#define PIN_QSPI_CS    XX
#define PIN_QSPI_SCLK  XX
#define PIN_QSPI_D0    XX
#define PIN_QSPI_D1    XX
#define PIN_QSPI_D2    XX
#define PIN_QSPI_D3    XX
#define PIN_I2C_SDA    XX
#define PIN_I2C_SCL    XX

static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

static void display_power_on_rail() {
  // Copy verbatim from hello_amoled::power_on_display_rail().
}

static void display_pulse_reset() {
  // Copy verbatim from hello_amoled::pulse_display_reset().
}

void display_begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  display_power_on_rail();
  display_pulse_reset();

  s_bus = new Arduino_ESP32QSPI(
      PIN_QSPI_CS, PIN_QSPI_SCLK,
      PIN_QSPI_D0, PIN_QSPI_D1, PIN_QSPI_D2, PIN_QSPI_D3);

  s_gfx = new Arduino_CO5300(
      s_bus, GFX_NOT_DEFINED /* external reset */,
      0 /* rotation */, false /* ips */,
      PANEL_WIDTH, PANEL_HEIGHT,
      0 /* col_offset1 */, 0 /* row_offset1 */,
      0 /* col_offset2 */, 0 /* row_offset2 */);

  if (!s_gfx->begin(40000000)) {
    Serial.println("display_begin: FAILED");
    while (true) { delay(1000); }
  }
}

void display_setBrightness(uint8_t value) {
  if (s_gfx) s_gfx->Display_Brightness(value);
}

void display_fillScreen(uint16_t color) {
  if (s_gfx) s_gfx->fillScreen(color);
}

void display_startWrite() {
  if (s_gfx) s_gfx->startWrite();
}

void display_endWrite() {
  if (s_gfx) s_gfx->endWrite();
}

void display_setAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (s_gfx) s_gfx->setAddrWindow(x, y, w, h);
}

// Push `len` RGB565 pixels (already byte-swapped big-endian, to match the
// convention default_large.h + the renderer use).
void display_writePixels(uint16_t *data, uint32_t len) {
  if (s_gfx) s_gfx->writePixels(data, len);
}
```

- [ ] **Step 2: (Does not compile yet)**

At this point the project still cannot compile — `uncanny-eyes-skull.ino` and `eye_functions.ino` still reference `TFT_eSPI`. That is fixed in Task 6. Do not try to compile yet.

- [ ] **Step 3: Commit**

```bash
git add display.ino
git commit -m "feat(display): add Arduino_GFX wrapper for CO5300 QSPI AMOLED"
```

---

## Task 6: Rewrite `uncanny-eyes-skull.ino` and trim `eye_functions.ino`

**Rationale:** This is the core port. These two files are deeply coupled (the main sketch owns the eye array and pbuffers; the renderer consumes them), so doing them in one atomic change is safer than trying to split — an intermediate split would be half-renamed and wouldn't compile anyway.

**Files:**
- Modify (overwrite): `uncanny-eyes-skull.ino`
- Modify: `eye_functions.ino` (specific blocks below)

- [ ] **Step 1: Overwrite `uncanny-eyes-skull.ino`**

```cpp
// Uncanny Eyes — Waveshare ESP32-S3-Touch-AMOLED-1.75 port (v1).
//
// Renders one eye (EYE_SIDE in config.h) as a 240x240 image centered on
// the 466x466 CO5300 AMOLED. See docs/superpowers/specs for the design.

#include "config.h"

// Ping-pong pixel buffers drained by display_writePixels().
#define BUFFER_SIZE 1024
#define BUFFERS 2
uint16_t pbuffer[BUFFERS][BUFFER_SIZE];
bool dmaBuf = 0;

// Blink state machine shared with eye_functions.ino.
#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2
struct eyeBlink {
  uint8_t  state;
  uint32_t duration;
  uint32_t startTime;
};

// One eye in v1. Kept as a struct to stay close to the original shape.
struct EyeState {
  eyeBlink blink;
} eye;

uint32_t startTime;

// Forward declarations (defined in eye_functions.ino / display.ino).
void     initEyes();
void     updateEye();

void     display_begin();
void     display_setBrightness(uint8_t);
void     display_fillScreen(uint16_t);
void     display_startWrite();
void     display_endWrite();
void     display_setAddrWindow(int16_t, int16_t, int16_t, int16_t);
void     display_writePixels(uint16_t *, uint32_t);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("uncanny-eyes: boot");

  initEyes();

  Serial.println("uncanny-eyes: display_begin()");
  display_begin();
  display_fillScreen(0x0000);
  display_setBrightness(DISPLAY_BRIGHTNESS);

  startTime = millis();
  Serial.println("uncanny-eyes: running");
}

void loop() {
  updateEye();
}
```

- [ ] **Step 2: Modify `eye_functions.ino`**

Apply the following transformations. Every transformation is justified in a one-line comment so a reviewer can reconstruct the reasoning.

**2a. Delete the "Adafruit-era" header comment block** (lines ~1–22 of the old file) — it refers to Teensy / Feather hardware the port no longer targets. Replace with:

```cpp
// Scanline-streaming renderer for the Uncanny Eyes.
// Reads baked eye graphics (sclera / iris / upper / lower) from PROGMEM,
// computes per-pixel values, and pushes them through display_writePixels()
// in 1024-pixel chunks. Single-eye, single-display variant.
```

**2b. Replace `initEyes()`** (old body iterated over `NUM_EYES` and set `tft_cs` / wink pins) with:

```cpp
void initEyes() {
  Serial.println("initEyes: single eye v1");
  eye.blink.state = NOBLINK;
}
```

**2c. Replace `updateEye()`** — old body had a `LIGHT_PIN` branch. With `LIGHT_PIN` gone, just the autonomous-iris branch remains:

```cpp
void updateEye() {
  newIris = random(IRIS_MIN, IRIS_MAX);
  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}
```

(The `oldIris` / `newIris` globals at the top of the file stay — they are the autonomous-iris scratch state.)

**2d. Replace the opening of `drawEye()`** (the part that does `digitalWrite(cs)` + `tft.startWrite()` + `tft.setAddrWindow(...)`) with the wrapper calls:

```cpp
void drawEye(
    uint32_t iScale,
    uint32_t scleraX, uint32_t scleraY,
    uint32_t uT, uint32_t lT) {
  uint32_t screenX, screenY, scleraXsave;
  int32_t  irisX, irisY;
  uint32_t p, a, d;
  uint32_t pixels = 0;

  display_startWrite();
  display_setAddrWindow(EYE_RENDER_X, EYE_RENDER_Y,
                        EYE_RENDER_WIDTH, EYE_RENDER_HEIGHT);
```

Note: the `uint8_t e` parameter is **gone**. The renderer now draws THE eye, not eye #e.

**2e. Replace the left/right mirror setup** (old code: `uint16_t lidX = 0; uint16_t dlidX = -1; if (e) dlidX = 1;` and inside the loop `if (e) lidX = 0; else lidX = SCREEN_WIDTH - 1;`) with code driven by `EYE_SIDE`:

```cpp
  // Eyelid map direction depends on which eye side this board is.
  // RIGHT eye walks the eyelid map left-to-right; LEFT eye walks it
  // right-to-left (caruncle on the nose-side).
  const uint16_t lidX_start = (EYE_SIDE == EYE_SIDE_LEFT) ? (SCREEN_WIDTH - 1) : 0;
  const int16_t  lidX_step  = (EYE_SIDE == EYE_SIDE_LEFT) ? -1 : 1;

  scleraXsave = scleraX;
  irisY       = scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  for (screenY = 0; screenY < SCREEN_HEIGHT; screenY++, scleraY++, irisY++) {
    scleraX = scleraXsave;
    irisX   = scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    uint16_t lidX = lidX_start;
    for (screenX = 0; screenX < SCREEN_WIDTH; screenX++,
                                              scleraX++, irisX++, lidX += lidX_step) {
      // ... body of inner loop stays as in the original (eyelid / iris /
      //     sclera resolution) ...
      *(&pbuffer[dmaBuf][0] + pixels++) = p >> 8 | p << 8;

      if (pixels >= BUFFER_SIZE) {
        yield();
        display_writePixels(&pbuffer[dmaBuf][0], pixels);
        dmaBuf = !dmaBuf;
        pixels = 0;
      }
    }
  }
```

Note: `SCREEN_WIDTH` / `SCREEN_HEIGHT` are defined in `data/default_large.h` — keep them as-is; they equal 240.

**2f. Replace the tail of `drawEye()`** (old: `tft.pushPixelsDMA(...) / tft.pushPixels(...) / tft.endWrite() / digitalWrite(cs, HIGH)`) with:

```cpp
  if (pixels) {
    display_writePixels(&pbuffer[dmaBuf][0], pixels);
  }
  display_endWrite();
}
```

**2g. Simplify `frame()`:**

- Remove the `eyeIndex` cycling (`if (++eyeIndex >= NUM_EYES) eyeIndex = 0;`) and replace every `eye[eyeIndex]` with `eye` (the single instance).
- Delete the entire `#if defined(JOYSTICK_X_PIN) …` block — keep only the `#else` autonomous-motion branch (without the `#else` / `#endif`).
- In the blink state machine: replace `eyeInfo[eyeIndex].wink` references with `-1` / remove the wink branch entirely (no wink pin in v1).
- Remove `BLINK_PIN` branches entirely (no blink button in v1).
- The X-convergence offset (old: `if (NUM_EYES > 1) { if (eyeIndex == 1) eyeX += 4; else eyeX -= 4; }`) becomes:

```cpp
  // Slight convergence so a pair of eyes looks fixated. +/- 4 px nudge
  // based on which side this board drives.
  eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4;
  if (eyeX > (SCLERA_WIDTH - 240)) eyeX = (SCLERA_WIDTH - 240);
  if (eyeX < 0) eyeX = 0;
```

- The `drawEye(...)` call at the bottom of `frame()` loses its first argument (`eyeIndex`):

```cpp
  drawEye(iScale, eyeX, eyeY, n, lThreshold);
```

- Delete the `if (eyeIndex == (NUM_EYES - 1)) { user_loop(); }` line entirely (no user-hook in v1).

**2h. Leave the `split()` function unchanged** (it only calls `frame()`, which still exists).

- [ ] **Step 3: Compile**

Arduino IDE → **Verify** (or `arduino-cli compile --fqbn <FQBN> .`).

Expected: compile succeeds with no errors and no warnings referencing `TFT_eSPI`, `tft`, `NUM_EYES`, `eyeInfo`, `user_setup`, `user_loop`, `LIGHT_PIN`, `JOYSTICK_*`, `BLINK_PIN`, or `wink`.

If the compile surfaces any residual reference to those symbols, remove that block — it's a leftover from the old code that 2b–2g should have covered. Do not reintroduce any of them.

- [ ] **Step 4: Commit**

```bash
git add uncanny-eyes-skull.ino eye_functions.ino
git commit -m "refactor(eyes): port renderer to Arduino_GFX, collapse to single eye"
```

---

## Task 7: Flash, verify the eye renders, measure FPS

**Rationale:** This is the end-to-end acceptance gate. Every success criterion in the spec is checked here. If anything fails, we fix it — but only within the existing architecture, not by re-opening design decisions.

**Files:** none modified directly. This task produces evidence (serial-log snippet + photo of the panel) that v1 works.

- [ ] **Step 1: Upload to the board**

Connect Waveshare ESP32-S3-Touch-AMOLED-1.75 via USB-C. Arduino IDE → **Upload** (or `arduino-cli upload --fqbn <FQBN> -p /dev/tty.usbmodem* .`).

- [ ] **Step 2: Open serial monitor at 115200 baud**

Expected serial output within ~2 s of reset:

```
uncanny-eyes: boot
initEyes: single eye v1
uncanny-eyes: display_begin()
uncanny-eyes: running
FPS=<n>
FPS=<n>
...
```

Every 256 frames a new `FPS=<n>` line is logged.

- [ ] **Step 3: Visually verify the eye**

Look at the round AMOLED. Expected:
- A single eye (`default_large`) occupies the central 240×240 square of the panel. The area outside it is black.
- The eye moves autonomously to new gaze points, with smooth easing.
- The iris scales smoothly over several seconds.
- The upper eyelid tracks the pupil (lid comes down more when the eye looks down).
- The eye blinks at random intervals (because `AUTOBLINK` is defined).
- No flicker, no visible tearing, no freeze.

- [ ] **Step 4: Record measured FPS**

Watch at least three `FPS=<n>` lines. Take the median. Acceptance: **≥ 20**. If 20 ≤ FPS < 32, accept and note for v2 to revisit. If FPS < 20, do **not** mark v1 done — see Step 5.

- [ ] **Step 5: Troubleshoot (only if Step 3 or 4 fails)**

| Symptom                                    | First thing to try                                                  |
| ------------------------------------------ | ------------------------------------------------------------------- |
| Panel stays black                          | Re-check `display_power_on_rail()` and `display_pulse_reset()`.     |
| Eye visible but offset by a few pixels     | Adjust `col_offset` / `row_offset` in `display.ino`'s `Arduino_CO5300` constructor; record final values in `docs/hardware-notes.md`. |
| Colors inverted (red/blue swapped)         | Remove the `p >> 8 \| p << 8` byte-swap in `drawEye()` — or keep it and flip the `bigEndian` argument on `writePixels` if that path is available. |
| Image mirrored left/right                  | You have `EYE_SIDE` set to the wrong side for this board. Flip the `#define` in `config.h`. |
| FPS < 20                                   | Raise QSPI clock in `display_begin()` from 40 MHz to 60 / 75 / 80 MHz (one step at a time, keep the highest that's glitch-free). If still low, increase `BUFFER_SIZE` to 2048. |
| Eyelid asymmetry looks wrong               | Re-check the `lidX_start` / `lidX_step` logic for the current `EYE_SIDE`; compare to the old 2-eye renderer's behavior for `e == 0`. |

Do not change `drawEye()`'s per-pixel resolution logic.

- [ ] **Step 6: Capture evidence**

Paste the serial log snippet (3+ FPS lines) into the commit message. Optionally take a photo of the running panel (not required to land the commit, but nice for the README).

- [ ] **Step 7: Commit**

Nothing changes code-wise, but commit the record:

```bash
git commit --allow-empty -m "test(v1): hardware verification — eye renders at <N> FPS

$(serial log snippet)"
```

---

## Task 8: Rewrite `README.md` for the new hardware

**Rationale:** The current `README.md` documents the old hardware end-to-end (dual GC9A01 wiring table, USB-cable-surgery, jaw servo steps). None of it applies. A reader arriving fresh should learn: what hardware is needed, what to install, what to flash.

**Files:**
- Modify (overwrite): `README.md`

- [ ] **Step 1: Overwrite `README.md`**

```markdown
# Uncanny Eyes — Waveshare ESP32-S3 AMOLED edition

An Arduino port of the classic [Adafruit Uncanny Eyes](https://github.com/adafruit/Uncanny_Eyes/)
demo to the **Waveshare ESP32-S3-Touch-AMOLED-1.75** round 466×466 AMOLED board.

v1 status: one board renders a single eye (left), autonomously moving, blinking,
and tracking eyelids. The eye is rendered at its native 240×240 resolution,
centered on the round panel.

## Hardware

- 1 × [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)
- 1 × USB-C cable

No servo, no external displays, no external wiring required for v1.

See [`docs/hardware-notes.md`](docs/hardware-notes.md) for the board's internal
pin map and peripheral init sequence (useful if you want to extend the sketch).

## Software prerequisites

- [Arduino IDE 2.x](https://www.arduino.cc/en/software)
- ESP32 core for Arduino 3.0 or newer (install via Boards Manager: search "esp32")
- [`GFX Library for Arduino`](https://github.com/moononournation/Arduino_GFX)
  (`Arduino_GFX`) by `moononournation`, installed via Library Manager

## Build & flash

1. Open `uncanny-eyes-skull.ino` in Arduino IDE.
2. Select **Tools → Board → esp32 → ESP32S3 Dev Module**.
3. Set the following **Tools** menu values:
   - USB CDC On Boot: **Enabled**
   - USB Mode: **Hardware CDC and JTAG**
   - Flash Mode: **QIO 80MHz**
   - Flash Size: **16MB (128Mb)**
   - PSRAM: **OPI PSRAM**
4. Plug in the board via USB-C. Select the correct port under **Tools → Port**.
5. Click **Upload**.
6. Open **Serial Monitor** at 115200 baud to see `FPS=<n>` output.

## Configuring left vs right eye

This firmware is intended to run on each eye-board independently. The
compile-time constant `EYE_SIDE` in `config.h` picks which eye this board
renders:

```c
#define EYE_SIDE EYE_SIDE_LEFT   // or EYE_SIDE_RIGHT
```

Flash the left-eye build to one board and the right-eye build to another. In
v1, the two boards do not communicate — they each animate independently.
(See `docs/superpowers/specs/` for the planned v2 two-board sync design.)

## Repo layout

| Path                                   | What it is                                       |
| -------------------------------------- | ------------------------------------------------ |
| `uncanny-eyes-skull.ino`               | main sketch — `setup()` / `loop()`               |
| `config.h`                             | per-board compile-time config                    |
| `eye_functions.ino`                    | eye animation state machine + renderer           |
| `display.ino`                          | Arduino_GFX wrapper for the CO5300 AMOLED        |
| `data/default_large.h`                 | baked eye graphics (sclera / iris / eyelids)     |
| `tools/hello_amoled/`                  | display bring-up smoke test                      |
| `docs/hardware-notes.md`               | Waveshare board pin map and init notes           |
| `docs/superpowers/specs/` & `plans/`   | design docs and implementation plans             |

## Credits

- Original Uncanny Eyes concept: Adafruit / Phil Burgess.
- Original 240-px ESP32 + GC9A01 port: the author this fork took over from.
- This port: v1 rewrite for the Waveshare ESP32-S3-Touch-AMOLED-1.75.

## License

MIT — see [`LICENSE`](LICENSE).
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs(readme): rewrite for Waveshare ESP32-S3 AMOLED target"
```

---

## Self-Review

After all tasks are drafted, sanity-check against the spec.

**1. Spec coverage:**

| Spec section                | Covered by                                  |
| --------------------------- | ------------------------------------------- |
| Goals #1 (Arduino IDE build)| Task 2, 6, 7                                |
| Goals #2 (autonomous anim)  | Task 6 (preserves renderer), Task 7 (verify)|
| Goals #3 (240×240 centered) | Task 4 (EYE_RENDER_X/Y), Task 6 (window)    |
| Goals #4 (≥20 FPS)          | Task 7 Step 4                               |
| Goals #5 (clean baseline)   | Tasks 3, 4, 6                               |
| File layout                 | Tasks 3, 4, 5, 6, 8                         |
| EYE_SIDE                    | Task 4 (define), Task 6 (lid + convergence) |
| Known risk: TCA9554 reset   | Task 1 (record), Task 2 (verify), Task 5 (port) |
| Known risk: AXP2101 rail    | Task 1, 2, 5                                |
| Known risk: QSPI pin map    | Task 1, 2, 5                                |
| Known risk: frame rate      | Task 7 Step 5                               |
| Success criteria            | Task 7 (all steps)                          |

No spec section is uncovered.

**2. Placeholder scan:** The plan has no "TBD" / "TODO" / "implement later" / "add appropriate error handling". Every code block is complete or explicitly marked as "copy from docs/hardware-notes.md" with a concrete reference. The `XX` placeholders in code for pin numbers are intentional and flagged — they cannot be filled in before Task 1 runs on a real hardware document.

**3. Type / name consistency:**
- `display_begin`, `display_setAddrWindow`, `display_writePixels`, `display_fillScreen`, `display_setBrightness`, `display_startWrite`, `display_endWrite` — same names in Task 5 (definition) and Task 6 (call sites).
- `EYE_SIDE`, `EYE_SIDE_LEFT`, `EYE_SIDE_RIGHT` — consistent across `config.h` (Task 4), `eye_functions.ino` (Task 6), and README (Task 8).
- `EYE_RENDER_X`, `EYE_RENDER_Y`, `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT` — defined in Task 4, used in Task 6.
- `pbuffer`, `dmaBuf`, `BUFFER_SIZE` — defined in `uncanny-eyes-skull.ino` (Task 6 Step 1), used in `eye_functions.ino` (Task 6 Step 2).
- `eye` (the single struct instance) — defined in Task 6 Step 1, used (via `eye.blink.*`) in Task 6 Step 2g.

All consistent.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-18-esp32-s3-amoled-port.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
