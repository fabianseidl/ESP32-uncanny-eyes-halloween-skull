# Uncanny Eyes on ESP32-S3 AMOLED

A port of the classic Adafruit "Uncanny Eyes" demo to the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (1.75" round 466×466 CO5300 AMOLED, QSPI). One eye per board; a second board can drive a second eye for a full pair (syncing is out of scope for v1).

Inspired by [Adafruit's Uncanny Eyes](https://github.com/adafruit/Uncanny_Eyes/). The renderer is descended from the [TFT_eSPI example](https://github.com/Bodmer/TFT_eSPI/), but the TFT_eSPI dependency has been removed — this port uses [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) for the CO5300 QSPI panel.

This repository previously targeted two 1.28" GC9A01 TFTs driven by an ESP32-WROOM-32D plus a servo-driven skull jaw. That hardware path has been removed. See the git history up to commit [`4dedaa3`](https://github.com/) if you need the old code.

## Scope (v2a renderer + v2b async QSPI)

- Renders **one eye** (left or right, selected at compile time) full-panel 466×466 on the AMOLED, NN-stretched from the 240-baked asset via a row expander. Per-pixel iris / sclera / eyelid logic still runs at source resolution (57.6K ops/frame); horizontal + vertical nearest-neighbour duplication happens at render resolution.
- Autonomous eye motion, autoblink, eyelid tracking, autonomous iris scaling — all preserved from the original.
- No touch, no IMU, no RTC, no PMU control, no audio. Those peripherals exist on the board but are not used yet.
- **v2b:** Pixels leave the MCU through a **second `spi_device_handle_t`** on `SPI2_HOST` (DMA queue + ping-pong buffers in `display_async.cpp`), while `Arduino_CO5300` / `Arduino_ESP32QSPI` keep the cold path (`setAddrWindow`, init, `fillScreen`, brightness). The library bus is opened for sharing (`is_shared_interface=true`). See `docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md`.
- **Measured FPS (same board, `default_large.h`, serial `FPS=`):** v2a baseline **~10 FPS** (QSPI-bound, synchronous `writePixels`). v2b **~17 FPS** after bring-up — a clear gain, still **below** the v2b spec stretch floor of 20 FPS; further CPU/DMA tuning is explicitly deferred (see *Ideas for later*).

### v2b hardware verification (merge checklist)

| Item | Expected |
|------|-----------|
| `arduino-cli compile` (FQBN below) | clean |
| Serial after `display_begin()` | `qspi_async: init ok` |
| Panel | full-panel eye, motion/blink/iris as v2a |
| Serial `FPS=` | ~17 on current build (re-measure after changes) |

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
qspi_async: init ok
uncanny-eyes: running
FPS=17
...
```

(`FPS=` varies with firmware; v2b full-panel builds have measured in the high teens on the reference board.)

The eye fills the full 466×466 AMOLED (the panel's own round mask trims the corners).

> **Tip:** the Waveshare's USB serial port enumerates under different `/dev/cu.usbmodemXXXX` names across reboots; re-query `arduino-cli board list` if upload fails with "port busy or doesn't exist."

## Picking left vs. right eye

Edit `config.h`:

```c
#define EYE_SIDE       EYE_SIDE_LEFT   // or EYE_SIDE_RIGHT
```

This controls two things: the eyelid-map mirror direction (so the caruncle ends up on the nose side), and a small ±4 px X-offset so a pair of boards looks convergently fixated.

## Eye gallery (compile-time)

Switch styles by editing **`data/eye_asset.h`**: keep **exactly one** active `#include` and comment the rest. Rebuild and reflash — only one eye’s tables are linked per binary (no duplicate symbols).

| Active include | Notes |
|----------------|--------|
| `data/default_large.h` | **Default.** 240²-baked hazel; sharpest on the 466 panel. |
| `data/defaultEye.h` | Classic 128² human hazel. |
| `data/dragonEye.h` | Slit pupil / demon. |
| `data/noScleraEye.h` | Large iris, minimal sclera. |
| `data/goatEye.h` | Horizontal pupil. |
| `data/newtEye.h` | “Eye of newt”. |
| `data/terminatorEye.h` | Red robot eye. |
| `data/catEye.h` | Cartoon cat. |
| `data/owlEye.h` | Owl — Adafruit recommends **disabling** `#define TRACKING` in `config.h` for this asset (comment out that line, then restore for other eyes). |
| `data/naugaEye.h` | Googly eye. |
| `data/doeEye.h` | Cartoon deer. |

128² assets are nearest-neighbour upscaled to 466² by the v2a row expander; expect softer detail than `default_large.h`. Runtime switching (touch) is planned as a later phase — see `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md`.

## Repository layout

```
ESP32-uncanny-eyes-halloween-skull.ino   Arduino entry point (setup/loop)
config.h                                 Per-board config (EYE_SIDE, geometry, flags)
display.ino                              Thin Arduino_GFX wrapper for CO5300 QSPI
display_async.cpp / display_async.h      DMA QSPI pixel stream (second SPI device)
eye_functions.ino                        Scanline-streaming renderer + animation
data/default_large.h                   Default 240² eye assets (sclera / iris / lids / polar)
data/*.h, data/eye_asset.h             Compile-time eye gallery (see *Eye gallery* above)

tools/hello_amoled/                      Standalone RGB smoke test for the panel
docs/hardware-notes.md                   Waveshare pin map + init notes
docs/superpowers/specs/                  Design specs (v2a row-expand, v2b async QSPI, …)
docs/superpowers/plans/                  Implementation plans
```

Only `display.ino` pulls in `Arduino_GFX`. The renderer uses `display_*` helpers in `display.ino` for the cold path and the C API in `display_async.h` (`display_pixelsBegin`, `display_pixelsQueueChunk`, `display_pixelsEnd`) for the hot pixel stream. Swapping displays still centers on `display.ino` + `display_async.*` + `config.h`.

## Known limitations / ideas for later

- **One eye only.** Two boards would need sync (e.g. ESP-NOW exchanging a shared RNG seed) so both eyes look at the same thing.
- **Nearest-neighbour upscale.** v2a fills the panel via integer-Bresenham NN duplication from the 240-baked asset; the scale factor (~1.94×) means obvious row/column repeats. A bilinear expander or a native-466 asset would look sharper. See `docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md` "Future Work".
- **v2b FPS headroom (~17 vs ≥20 spec floor, 30 stretch).** Deferred optimizations (no commitment in this merge): `esp_timer` phase profiling to find the dominant cost; fold RGB565 byte-swap into `expandRow` to drop a pass; trim per-chunk `memset` of `spi_transaction_ext_t`; optional `#ifdef` toggles for `TRACKING` / `IRIS_SMOOTH` cost experiments; dirty-rect / smaller address windows per `docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md` "Future work".
- **TCA9554 expander, AXP2101 PMU, CST9217 touch, QMI8658 IMU, PCF85063 RTC** — all present on the board, all unused. See `docs/hardware-notes.md` for pins / I²C addresses when you want to light them up.
