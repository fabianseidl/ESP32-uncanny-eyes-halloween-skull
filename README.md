# Uncanny Eyes on ESP32-S3 AMOLED

A port of the classic Adafruit "Uncanny Eyes" demo to the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (1.75" round 466×466 CO5300 AMOLED, QSPI). One eye per board; a second board can drive a second eye for a full pair (syncing is out of scope for v1).

Inspired by [Adafruit's Uncanny Eyes](https://github.com/adafruit/Uncanny_Eyes/). The renderer is descended from the [TFT_eSPI example](https://github.com/Bodmer/TFT_eSPI/), but the TFT_eSPI dependency has been removed — this port uses [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) for the CO5300 QSPI panel.

This repository previously targeted two 1.28" GC9A01 TFTs driven by an ESP32-WROOM-32D plus a servo-driven skull jaw. That hardware path has been removed. See the git history up to commit [`4dedaa3`](https://github.com/) if you need the old code.

## Scope (v2a renderer + v2b async QSPI)

- Renders **one eye** (left or right, selected at compile time) full-panel 466×466 on the AMOLED, NN-stretched from the 240-baked asset via a row expander. Per-pixel iris / sclera / eyelid logic still runs at source resolution (57.6K ops/frame); horizontal + vertical nearest-neighbour duplication happens at render resolution.
- Autonomous eye motion, autoblink, eyelid tracking, autonomous iris scaling — all preserved from the original.
- **Runtime eye gallery:** six 128² styles baked in flash (`tools/gen_eye_gallery_bundles.py` → `eye_gallery_bundles.cpp`); **tap** the panel (CST9217) or send **`n`** / newline on serial to cycle. Requires [SensorLib](https://github.com/lewisxhe/SensorLib) for touch (see prerequisites). No IMU, RTC, PMU, or audio.
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
- **SensorLib** (Lewis He) — CST9217 touch for on-panel style cycling. Install once:
  ```bash
  arduino-cli lib install "SensorLib"
  ```
  If SensorLib is missing, the sketch still compiles; touch is disabled and serial `n` remains.

## Build and flash

The sketch folder must match the main `.ino` filename, so always run `arduino-cli` against the repo root `.`, not against an individual file.

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"
PORT=$(arduino-cli board list | awk '/ESP32 Family Device/{print $1; exit}')

arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p "$PORT" .
arduino-cli monitor -p "$PORT" -c baudrate=115200
```

> **Why `huge_app`?** With pair sync (`EYE_SYNC_ENABLE 1`, default) the WiFi + ESP-NOW stack is linked, and the binary (~1.83 MB) overflows the standard `default` 1.31 MB app partition. `huge_app` provides a ~3 MB app partition and drops the unused OTA slot. Building with `EYE_SYNC_ENABLE 0` (single-eye fallback) fits in either partition scheme — see *Pair sync* below.

For pair-of-boards work, `flash_board1.sh` and `flash_board2.sh` at the repo root compile + upload + monitor the first / second `arduino-cli board list` entry — useful when both boards are plugged in simultaneously.

Expected serial output at 115200:

```
uncanny-eyes: boot
initEyes: runtime gallery v1
eye_gallery: start nauga
uncanny-eyes: display_begin()
qspi_async: init ok
eye_gallery: touch ok CST9217
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

## Eye gallery (runtime, default firmware)

The main sketch links **six** 128² styles at once (PROGMEM tables renamed by the generator). Order and membership are **`SPECS`** in `tools/gen_eye_gallery_bundles.py` (default: nauga, owl, cat, goat, terminator, newt). Regenerate after edits:

```bash
python3 tools/gen_eye_gallery_bundles.py
```

Then `arduino-cli compile` as usual. On the default app partition, **six** is near the flash ceiling (~94%); a seventh style typically overflows — use a larger app partition or trim `SPECS`.

**Cycle styles:** short **tap** on the glass (release to advance) or serial **`n`**, **`N`**, **CR/LF**. Debounce ~400 ms between touch advances.

**Legacy compile-time gallery:** `data/eye_asset.h` is still useful for one-header experiments, but `config.h` in this branch uses the generated gallery limits + bundles, not `eye_asset.h`. To compare a single raw header build, switch `config.h` back to `#include "data/eye_asset.h"` and drop the gallery `line_src` sizing (see git history / phase 1 plan).

| Style (default runtime set) | File |
|----------------------------|------|
| nauga, owl, cat, goat, terminator, newt | `data/*Eye.h` |

**Owl:** with `owl` in the rotation, Adafruit recommends disabling `#define TRACKING` in `config.h` if eyelid tracking looks wrong.

Design: `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md` — phase 2 (touch + serial) implemented on branch `feat/adafruit-eye-gallery-phase2`.

## Repository layout

```
ESP32-uncanny-eyes-halloween-skull.ino   Arduino entry point (setup/loop)
config.h                                 Per-board config (EYE_SIDE, geometry, flags)
display.ino                              Thin Arduino_GFX wrapper for CO5300 QSPI
display_async.cpp / display_async.h      DMA QSPI pixel stream (second SPI device)
eye_functions.cpp                        Scanline-streaming renderer + animation
eyes.h                                   Shared blink state + init/update prototypes
eye_gallery.cpp / eye_gallery.h          Runtime style index, serial + touch poll
eye_runtime.h                            Per-style dimensions + PROGMEM pointers
eye_gallery_bundles.cpp                  Generated PROGMEM tables (run tools/gen_eye_gallery_bundles.py)
generated/eye_gallery_limits.h           Generated max source width/height
data/*.h, data/eye_asset.h               Per-eye headers; legacy single-include gallery

tools/hello_amoled/                      Standalone RGB smoke test for the panel
tools/gen_eye_gallery_bundles.py         Builds eye_gallery_bundles.cpp + limits header
docs/hardware-notes.md                   Waveshare pin map + init notes
docs/superpowers/specs/                  Design specs (v2a row-expand, v2b async QSPI, …)
docs/superpowers/plans/                  Implementation plans
```

Only `display.ino` pulls in `Arduino_GFX`. The renderer uses `display_*` helpers in `display.ino` for the cold path and the C API in `display_async.h` (`display_pixelsBegin`, `display_pixelsQueueChunk`, `display_pixelsEnd`) for the hot pixel stream. Swapping displays still centers on `display.ino` + `display_async.*` + `config.h`.

## Pair sync (phase C — gallery index over ESP-NOW)

When two boards run this firmware on the same WiFi channel, a tap on either board cycles **both** to the next gallery style. Sync uses ESP-NOW broadcast with a 4-byte magic prefix — no router, no AP, no extra Arduino library.

Configure in `config.h`:

```c
#define EYE_SYNC_ENABLE   1   // 0 = single-eye fallback (no WiFi linked)
#define EYE_SYNC_CHANNEL  1   // both boards must agree
```

Expected serial on each board (with `EYE_SYNC_LOG 1`):

```
eye_sync: init ok ch=1 mac=AA:BB:CC:DD:EE:FF
eye_sync: tx idx=0 flag=hb rc=0
eye_sync: rx idx=0 from=11:22:33:44:55:66 flag=hb
```

Tap on either board produces `flag=tap` on the sender and an arrow log (`eye_gallery: <- owl`) on the receiver. Asymmetric boot self-heals on the next heartbeat (≤ 2 s).

`eye_sync_tick()` is polled from inside `frame()` (alongside the existing touch poll) — `loop()` alone is too infrequent because `updateEye()` blocks for ~10 s per call, which would balloon RX latency. With the in-frame poll, RX latency is one render frame (~45 ms at ~22 FPS).

Animation sync (eyes looking at the same point, blinking together) is **phase B**, not in this build.

Design: `docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md`. Implementation: branch `feat/eye-sync-phase-c`.

## Known limitations / ideas for later

- **Animation sync (phase B).** Phase C (this build) keeps both boards on the same gallery style; both still animate independently. Phase B will sync eye motion / blink / iris, likely via a shared RNG seed broadcast on the same ESP-NOW transport.
- **Nearest-neighbour upscale.** v2a fills the panel via integer-Bresenham NN duplication from the 240-baked asset; the scale factor (~1.94×) means obvious row/column repeats. A bilinear expander or a native-466 asset would look sharper. See `docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md` "Future Work".
- **v2b FPS headroom (~17 vs ≥20 spec floor, 30 stretch).** Deferred optimizations (no commitment in this merge): `esp_timer` phase profiling to find the dominant cost; fold RGB565 byte-swap into `expandRow` to drop a pass; trim per-chunk `memset` of `spi_transaction_ext_t`; optional `#ifdef` toggles for `TRACKING` / `IRIS_SMOOTH` cost experiments; dirty-rect / smaller address windows per `docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md` "Future work".
- **TCA9554 expander, AXP2101 PMU, QMI8658 IMU, PCF85063 RTC** — present on the board, unused in this sketch. **CST9217** is used for gallery tap-to-advance. See `docs/hardware-notes.md` for pins and I²C addresses.
