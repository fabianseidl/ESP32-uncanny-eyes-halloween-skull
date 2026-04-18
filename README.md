# Uncanny Eyes on ESP32-S3 AMOLED

A port of the classic Adafruit "Uncanny Eyes" demo to the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (1.75" round 466×466 CO5300 AMOLED, QSPI). One eye per board; a second board can drive a second eye for a full pair (syncing is out of scope for v1).

Inspired by [Adafruit's Uncanny Eyes](https://github.com/adafruit/Uncanny_Eyes/). The renderer is descended from the [TFT_eSPI example](https://github.com/Bodmer/TFT_eSPI/), but the TFT_eSPI dependency has been removed — this port uses [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) for the CO5300 QSPI panel.

This repository previously targeted two 1.28" GC9A01 TFTs driven by an ESP32-WROOM-32D plus a servo-driven skull jaw. That hardware path has been removed. See the git history up to commit [`4dedaa3`](https://github.com/) if you need the old code.

## v2a scope

- Renders **one eye** (left or right, selected at compile time) full-panel 466×466 on the AMOLED, NN-stretched from the 240-baked asset via a row expander. Per-pixel iris / sclera / eyelid logic still runs at source resolution (57.6K ops/frame); horizontal + vertical nearest-neighbour duplication happens at render resolution.
- Autonomous eye motion, autoblink, eyelid tracking, autonomous iris scaling — all preserved from the original.
- No touch, no IMU, no RTC, no PMU control, no audio. Those peripherals exist on the board but are not used yet.
- v1 baseline (native 240×240, centered, no DMA queueing) measured **~32 FPS** on the Waveshare 1.75 at 40 MHz QSPI. v2a FPS is recorded per-build after hardware verification — see the design spec for the current measurement.

## Hardware

One per eye:

- [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm) — ESP32-S3R8 (8 MB PSRAM, 16 MB flash), CO5300 1.75" 466×466 AMOLED, USB-C.
- A USB-C cable for power and flashing.

No external wiring. Everything lives on the Waveshare board.

For a full pair of eyes you need two of these boards. Each board is flashed with the same firmware, compiled with either `EYE_SIDE_LEFT` or `EYE_SIDE_RIGHT` in `config.h`.

## Software prerequisites

- [`arduino-cli`](https://arduino.github.io/arduino-cli/) (works from VS Code / any terminal) — or the Arduino IDE 2.x GUI if you prefer.
- ESP32 core for Arduino, **≥ 3.3.5**. Install once:
  ```bash
  arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  arduino-cli core update-index
  arduino-cli core install esp32:esp32
  ```
- [`GFX Library for Arduino`](https://github.com/moononournation/Arduino_GFX) by moononournation. Install once:
  ```bash
  arduino-cli lib install "GFX Library for Arduino"
  ```

## Build and flash

The sketch folder must match the main `.ino` filename, so always run `arduino-cli` against the repo root `.`, not against an individual file.

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default"
PORT=$(arduino-cli board list | awk '/ESP32 Family Device/{print $1; exit}')

arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p "$PORT" .
arduino-cli monitor -p "$PORT" -c baudrate=115200
```

Expected serial output at 115200:

```
uncanny-eyes: boot
initEyes: single eye v1
uncanny-eyes: display_begin()
uncanny-eyes: running
FPS=32
...
```

The eye fills the full 466×466 AMOLED (the panel's own round mask trims the corners).

> **Tip:** the Waveshare's USB serial port enumerates under different `/dev/cu.usbmodemXXXX` names across reboots; re-query `arduino-cli board list` if upload fails with "port busy or doesn't exist."

## Picking left vs. right eye

Edit `config.h`:

```c
#define EYE_SIDE       EYE_SIDE_LEFT   // or EYE_SIDE_RIGHT
```

This controls two things: the eyelid-map mirror direction (so the caruncle ends up on the nose side), and a small ±4 px X-offset so a pair of boards looks convergently fixated.

## Repository layout

```
ESP32-uncanny-eyes-halloween-skull.ino   Arduino entry point (setup/loop)
config.h                                 v1 per-board config (EYE_SIDE, geometry, flags)
display.ino                              Thin Arduino_GFX wrapper for CO5300 QSPI
eye_functions.ino                        Scanline-streaming renderer + animation
data/default_large.h                     Baked 240x240 eye graphics (sclera/iris/lids)

tools/hello_amoled/                      Standalone RGB smoke test for the panel
docs/hardware-notes.md                   Waveshare pin map + init notes
docs/superpowers/specs/                  v1 design doc
docs/superpowers/plans/                  v1 implementation plan
```

Only `display.ino` knows about `Arduino_GFX`; the renderer talks to it through a tiny C function API (`display_begin`, `display_setAddrWindow`, `display_writePixels`, …). Swapping to a different display library or board should only touch `display.ino` + `config.h`.

## Known limitations / ideas for later

- **One eye only.** Two boards would need sync (e.g. ESP-NOW exchanging a shared RNG seed) so both eyes look at the same thing.
- **Nearest-neighbour upscale.** v2a fills the panel via integer-Bresenham NN duplication from the 240-baked asset; the scale factor (~1.94×) means obvious row/column repeats. A bilinear expander or a native-466 asset would look sharper. See `docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md` "Future Work".
- **No DMA in the wrapper.** `display.writePixels()` is synchronous. Queued / double-buffered DMA through `Arduino_ESP32QSPI`'s async API would likely push FPS past 50.
- **TCA9554 expander, AXP2101 PMU, CST9217 touch, QMI8658 IMU, PCF85063 RTC** — all present on the board, all unused. See `docs/hardware-notes.md` for pins / I²C addresses when you want to light them up.
