# Port Uncanny Eyes to Waveshare ESP32-S3-Touch-AMOLED-1.75 — v1 design

**Status:** draft, pending user review
**Date:** 2026-04-18
**Scope:** v1 only. Future work is listed but not designed here.

## Summary

Port the existing Uncanny Eyes sketch from the old target (ESP32 classic + two GC9A01 240×240 SPI displays, `TFT_eSPI`) to one Waveshare **ESP32-S3-Touch-AMOLED-1.75** board (ESP32-S3R8 + CO5300 QSPI AMOLED at 466×466). v1 renders a single **left** eye, autonomously animating, as a 240×240 image centered on the 466×466 panel. v2 and later will add an upscaled full-panel render, a second synchronized board for the right eye, and optional use of touch / IMU / audio.

The existing dual-display code path, the jaw servo, and the old ESP32 target are removed — they are no longer in scope for the project and kept them as carrying dead code.

## Goals

1. One Arduino sketch, built with Arduino IDE, flashes to one Waveshare ESP32-S3-Touch-AMOLED-1.75 board.
2. On power-up the board autonomously renders the `default_large` left eye: eye motion, autoblink, eyelid tracking, autonomous iris scaling.
3. The eye is a 240×240 image **centered** on the 466×466 round AMOLED (origin at `(113, 113)`). The corners outside the eye render as black.
4. Frame rate ≥ 20 FPS sustained (stretch: match or exceed the old ESP32 + DMA of ~32 FPS).
5. Code is a clean baseline — no leftover ESP32-classic / GC9A01 / jaw / 2-eye logic.

## Non-goals for v1

- Upscaling the eye image to 466×466 (v2).
- Driving a second eye on a second board, whether mirrored or synchronized (v2+).
- Using the CST9217 capacitive touch chip.
- Using the QMI8658 IMU.
- Using the dual microphones, ES7210 echo cancel chip, or the speaker header.
- Using the PCF85063 RTC or the TF card.
- Battery-powered operation as a first-class feature (may work incidentally via AXP2101 defaults, not verified here).
- Jaw servo or any other animatronic actuator.

## Target hardware (v1)

- Waveshare **ESP32-S3-Touch-AMOLED-1.75** (standard version, SKU 31261).
- USB-C power / flashing.

Relevant onboard chips (for this design, only the first two are actively used; the rest are listed so the design doesn't accidentally break them):

| Chip     | Role                                 | Bus            | Used in v1? |
| -------- | ------------------------------------ | -------------- | ----------- |
| CO5300   | AMOLED display driver (466×466)      | QSPI           | Yes         |
| AXP2101  | Power management (rails, battery)    | I²C            | Init only¹  |
| TCA9554  | GPIO expander (display RST, others)  | I²C            | Init only¹  |
| CST9217  | Capacitive touch                     | I²C            | No          |
| QMI8658  | 6-axis IMU                           | I²C            | No          |
| PCF85063 | RTC                                  | I²C            | No          |
| ES7210   | Audio echo cancel                    | I²C            | No          |

¹ AXP2101 and TCA9554 are used only to the minimum extent needed to bring up the CO5300 display (see "Known risks" below).

## Architecture

### File layout after v1

```
uncanny-eyes-skull.ino   main sketch — setup() / loop()
config.h                 compile-time configuration (EYE_SIDE, brightness, render origin)
eye_functions.ino        eye state machine + renderer (drawEye, frame, split)
display.ino              thin wrapper around Arduino_GFX: init, pushPixels, address-window helpers
data/default_large.h     baked eye graphics (only this asset kept; others deleted)
README.md                rewritten for new hardware
```

Files **deleted** in v1:
- `user_empty.cpp`, `user_skull-jaw.cpp` (jaw hook mechanism retired)
- `wiring/` folder (old ESP32 + GC9A01 wiring)
- `data/catEye.h`, `data/defaultEye.h`, `data/doeEye.h`, `data/dragonEye.h`, `data/goatEye.h`, `data/logo.h`, `data/naugaEye.h`, `data/newtEye.h`, `data/noScleraEye.h`, `data/owlEye.h`, `data/terminatorEye.h`

### Libraries

- `Arduino_GFX` by moononournation — provides `Arduino_ESP32QSPI` bus + `Arduino_CO5300` display class.

Removed from dependencies: `TFT_eSPI`, `ESP32Servo`.

### Configuration model (`config.h`)

Collapsed to just what v1 actually varies:

```cpp
// Which eye this board renders. Affects eyelid mirroring and X-convergence.
#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_LEFT   // v1: left eye

// Display geometry
#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466

// Eye render geometry (v1: native 240x240 centered, no scaling)
#define EYE_RENDER_WIDTH   240
#define EYE_RENDER_HEIGHT  240
#define EYE_RENDER_X  ((PANEL_WIDTH  - EYE_RENDER_WIDTH ) / 2)  // 113
#define EYE_RENDER_Y  ((PANEL_HEIGHT - EYE_RENDER_HEIGHT) / 2)  // 113

// AMOLED brightness (0..255). AMOLED has no backlight; this is CO5300 0x51.
#define DISPLAY_BRIGHTNESS 200

// Behavior toggles (carried over from old config, pared down)
#define TRACKING              // eyelid tracks pupil
#define AUTOBLINK             // blink at random intervals (v1 enables this so the demo is livelier)
#define IRIS_SMOOTH

// Iris range overrides are picked up from the eye asset header itself.
```

Knobs removed vs old config: `TFT_COUNT`, `TFT1_CS`, `TFT2_CS`, `TFT_1_ROT`, `TFT_2_ROT`, `EYE_1_XPOSITION`, `EYE_2_XPOSITION`, `DISPLAY_BACKLIGHT`, `BACKLIGHT_MAX`, `NUM_EYES`, `BLINK_PIN`, `LH_WINK_PIN`, `RH_WINK_PIN`, `eyeInfo[]`, `JOYSTICK_*`, `LIGHT_PIN`. The joystick / light-sensor / wink-button input paths stay out of the compiled sketch in v1 — they can be re-added later as first-class optional modules, not as a tangle of `#ifdef`s.

### Rendering pipeline

Data flow per frame, on the single core running `loop()`:

```
frame()
 └─ computes eyeX, eyeY, uThreshold, lThreshold, iScale for this frame
    └─ drawEye(iScale, eyeX, eyeY, uT, lT)
        ├─ display_setAddrWindow(EYE_RENDER_X, EYE_RENDER_Y, 240, 240)
        ├─ for each of 240 rows:
        │   for each of 240 cols:
        │       resolve pixel: eyelid | iris | sclera (unchanged logic)
        │       write into one of two 1024-pixel ping-pong buffers
        │       when buffer is full → display_pushPixelsDMA(buf)
        └─ flush remaining pixels
```

Key differences from the old implementation:

- `tft` object (`TFT_eSPI`) replaced with a module-local `Arduino_CO5300 *gfx` behind a small `display.ino` wrapper. The wrapper exposes exactly what `drawEye()` needs (`display_begin`, `display_setAddrWindow`, `display_pushPixelsDMA`, `display_fillScreen`, `display_setBrightness`). No other file talks to `Arduino_GFX` directly.
- No `tft_cs` per eye. There is one display.
- No per-eye iteration. `frame()` is always called for the single configured eye. The left/right distinction lives in the renderer (eyelid-mirror direction, X-convergence sign) driven by `EYE_SIDE`.
- Pixel byte-swap (`p >> 8 | p << 8`) is preserved — `default_large.h` is big-endian RGB565.

The `drawEye()` math — eyelid threshold sampling, polar→iris lookup, sclera fallback — is **unchanged**.

### Address window mapping on CO5300

CO5300 is a 466×466 AMOLED. Some members of this family (and the specific panel on this board) have an internal memory origin offset — for example, a 466-row panel may be accessible at rows 0..465 or at rows 6..471 depending on how the panel is wired/configured. `Arduino_GFX`'s `Arduino_CO5300` class encodes the correct offsets for this board in its begin sequence; v1 trusts the library defaults. If v1 bring-up reveals a visible offset (eye shifted by a few pixels, edge cut off), we fix it in the `display_setAddrWindow` wrapper, not by forking the library.

## Key decisions

1. **Arduino IDE + `Arduino_GFX` over PlatformIO / ESP-IDF.** Lowest tooling change from the existing project. Same API shape as `TFT_eSPI`.
2. **Native 240×240 render, centered, no scaling for v1.** Keeps the renderer loop bit-for-bit compatible with the old, proven one. Upscaling is a separate, isolated change for v2.
3. **Delete rather than `#ifdef` old code paths.** The project's future is ESP32-S3 + AMOLED. Carrying GC9A01 / 2-eye / jaw code behind flags means every reader still has to understand them.
4. **Left / right eye is a per-board compile-time constant, not a pin.** Simpler than runtime selection, and the only difference between the two boards' firmwares is a one-line `#define`.
5. **No user-hook mechanism (`user_setup`/`user_loop`).** The original was there to plug in the jaw servo. With jaw gone, it's complexity without use. If v2 adds IMU or touch, they plug in as named modules, not anonymous hooks.
6. **v1 explicitly does not design the two-board sync protocol.** Locking that in now would over-constrain v2. We only commit to: the left/right distinction is a `#define`, so v2's sync layer can assume identical firmware per eye modulo that constant.

## Known risks / things to verify during v1 bring-up

1. **Display reset via TCA9554.** On some Waveshare AMOLED boards the CO5300 `RESX` pin is not a direct ESP32 GPIO — it's behind the TCA9554 I²C GPIO expander. If so, calling `gfx->begin()` without first initializing TCA9554 and toggling the expander pin leaves the display stuck. **Verification**: check Waveshare's 1.75" demo sketch / schematic; replicate whatever reset sequence it does. Arduino_GFX supports passing `GFX_NOT_DEFINED` for the reset pin and performing reset externally.
2. **Display power rail via AXP2101.** The AMOLED may be powered through a rail that AXP2101 does not enable by default after cold boot. **Verification**: check Waveshare demo for any AXP2101 register writes before display init; port those over.
3. **QSPI pin map.** The ESP32-S3 → CO5300 QSPI lines (CS, SCLK, D0..D3) are fixed on this board and must match Waveshare's schematic. **Verification**: copy pin assignments directly from Waveshare's Arduino demo, do not guess.
4. **Frame rate.** The old target hit ~32 FPS at 240×240 with SPI DMA. QSPI at 4× SPI bandwidth should comfortably exceed that, but the `Arduino_GFX` DMA path for QSPI on the S3 may not be as tuned as `TFT_eSPI`'s. **Mitigation**: if we fall short of 20 FPS, first try increasing `BUFFER_SIZE`, then investigate `Arduino_GFX`'s DMA setting, before touching the renderer.
5. **PROGMEM vs PSRAM for eye assets.** `default_large.h` is a large `PROGMEM` table. On ESP32-S3 `PROGMEM` maps into flash and `pgm_read_*` still works. No change needed in v1, but worth verifying compiled size fits 16 MB flash (it will, easily).
6. **AMOLED burn-in.** AMOLED panels permanently degrade static pixels. v1 doesn't introduce static UI, but if the sketch ever sits idle on the same frame, burn-in is possible over hours. Not a v1 defect, but noted for future work.

## Success criteria (v1 "done")

All of the following on the target board:

- Flashing the built sketch from Arduino IDE produces no compile errors and no runtime crashes / brownouts.
- Within ~2 s of boot, the left eye appears, centered on the round 466×466 AMOLED panel.
- The eye autonomously moves to new gaze points with smooth easing.
- The iris scales autonomously.
- Eyelid tracks the pupil (upper lid closes more when the eye looks down, etc.).
- With `AUTOBLINK` defined, the eye blinks at random intervals.
- Measured FPS (serial log `FPS=<n>` line, already present in `frame()`) is ≥ 20.
- Serial log shows no repeated error lines.
- Code review: a new reader can open the repo and not find any reference to GC9A01, `TFT_eSPI`, `ESP32Servo`, jaw, or a second eye.

## Out of scope — explicit non-work list

- Upscale / regenerate 466-native eye assets.
- Two-board eye synchronization protocol.
- Touch-, IMU-, mic-, speaker-, RTC-, TF-driven features.
- Battery operation, low-power / sleep modes.
- Over-the-air update.
- Web / BLE configuration.

These are all future-v2+ and will get their own designs.

## Future work (sketch only, not committed)

- **v2a: Full-panel render.** Scale the eye renderer to 466×466 natively, either by regenerating `default_large.h` at a larger sclera/iris or by adding an integer-ratio upscaler in the scanline loop. Free 8 MB PSRAM makes a full 466×466×2 B back-buffer trivial.
- **v2b: Second eye.** Flash same firmware to a second Waveshare board with `EYE_SIDE = EYE_SIDE_RIGHT`. Add a sync channel (ESP-NOW is the leading candidate — peer-to-peer, low latency, no Wi-Fi AP needed) carrying `(gazeTargetX, gazeTargetY, blinkState, irisScale)`.
- **v2c: Touch to blink / IMU head-tracking / mic-triggered reactions.** Each as an independent, opt-in module.
