# v2a — Scale renderer to full 466×466 (NN, size-agnostic)

> **Superseded** by [`2026-04-18-v2a-row-expand-design.md`](2026-04-18-v2a-row-expand-design.md).
> This document describes an earlier v2a variant in which NN upscale is performed inside `drawEye()`'s scanline via Bresenham-driven source-index advance. It is kept for history only and is NOT the implementation target. The superseding spec moves upscale to a dedicated row-expander at the pixel-push stage, running per-pixel iris / sclera / lid decisions at source resolution.

**Status:** draft, pending user review — SUPERSEDED
**Date:** 2026-04-18
**Scope:** v2a only — full-panel render on one board. Two-board sync, native-466 assets, touch/IMU/audio, and battery remain future work.

## Summary

Upgrade the renderer so the eye fills the entire 466×466 CO5300 AMOLED panel, using the existing 240-baked `data/default_large.h` asset stretched nearest-neighbor. The scanline loop becomes **size-agnostic**: it iterates over render-space pixels (466×466) and reads source-space pixels (240×240) through a Bresenham-style integer accumulator. When source and render dims are equal, the mapping collapses to identity — so a future native-466 asset swaps in with zero renderer change.

v1's centered-240 render is removed. v2a is the new baseline.

## Goals

1. The eye fills the full 466×466 round AMOLED — no centered inset, no visible black border beyond the panel's own round mask.
2. Eye motion, autoblink, eyelid tracking, and iris scaling behave identically to v1 (same timings, easing, randomness profile).
3. Sustained FPS ≥ 10. Stretch target: ≥ 20.
4. The renderer makes **no assumption about source asset dimensions.** Any valid `data/*.h` asset header (120, 240, 350, 466, …) compiles and runs without touching `drawEye()`.
5. Left-eye and right-eye builds both render correctly — eyelid mirror direction and convergence-sign logic still work at render scale.

## Non-goals for v2a

- Bilinear or any non-NN interpolation.
- Regenerating eye assets at 466-native (future v2a-followup).
- A `CENTERED_240` fallback toggle. v1 lives in git history; `#ifdef` fallback paths are not carried.
- Renderer perf work beyond what's needed to clear the 10 FPS floor.
- Two-board sync, touch / IMU / audio / RTC / TF, battery operation, OTA — all still v2b+.

## Target hardware

Unchanged from v1: one Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466×466). No new chip usage.

## Architecture

### Coordinate systems

The core of the design is the explicit separation of two coordinate systems that v1 conflated:

- **Source-space** — pixel dimensions baked into the asset header. `data/default_large.h` defines `SCREEN_WIDTH = 240`, `SCREEN_HEIGHT = 240`, `SCLERA_WIDTH = 375`, `IRIS_WIDTH = 180`, `IRIS_HEIGHT = 180`, `IRIS_MAP_WIDTH = 489`. Eye motion, eyelid tracking, convergence, and iris lookups all live in source-space.
- **Render-space** — pixel dimensions of the panel draw window. `RENDER_WIDTH = RENDER_HEIGHT = 466`. Only `drawEye()` and `display_setAddrWindow()` know about render-space.

Render-space enters the codebase only at those two sites. Everything else reads source-space values directly from the asset header. This is the property that makes the renderer size-agnostic.

### Bresenham-style source-index advance

For each render axis we need: "advance the source index by `SRC / RENDER` per render step, exactly, using only integers." Canonical form, per-axis:

```c
int16_t src_x       = <start offset>;   // e.g. scleraXsave, lidX_start
int16_t src_x_accum = 0;
for (int16_t rx = 0; rx < RENDER_WIDTH; rx++) {
  // ... use src_x in pgm_read_* ...
  src_x_accum += SCREEN_WIDTH;           // numerator = source dim
  while (src_x_accum >= RENDER_WIDTH) {  // denominator = render dim
    src_x_accum -= RENDER_WIDTH;
    src_x += step;                       // +1 for most axes, -1 for left-eye lidX
  }
}
```

Properties:

- Exact integer math; no fixed-point precision to get wrong.
- Collapses to identity when `SRC == RENDER` — the `while` executes exactly once per step, `src_x++` every render pixel, bit-for-bit equivalent to v1's inner loop.
- Monotone (per axis), no back-tracking into PROGMEM arrays.
- The `while` becomes `if` in upscale cases (`RENDER ≥ SRC`), which is v2a's reality. Leaving it as `while` preserves correctness under downscale for free.
- Mirror direction flips cleanly — left-eye replaces `src_x += 1` with `src_x -= 1`; the accumulator math is unchanged.

### `drawEye()` structure

Per frame we maintain **one Y Bresenham accumulator** and **three independent X Bresenham accumulators** (one per source-X index that needs to advance at a different start offset or direction).

**Y axis** — single accumulator. In v1, `scleraY`, `irisY`, and `screenY` all advance once per source row in lockstep. That lockstep is preserved: the Y accumulator advances one "source row" at a time against the 466-high render loop, and the three Y indices all tick together when it does:

| Y source index | Start value (source-space)                        | Step per accumulator tick |
| -------------- | ------------------------------------------------- | ------------------------- |
| `scleraY`      | `scleraY` argument to `drawEye()` (gaze pan Y)    | +1                        |
| `irisY`        | `scleraY_arg - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2` | +1                        |
| `screenY`      | `0`                                               | +1                        |

The lid map is indexed as `[screenY][lidX]`, so `screenY` (the canonical source row counter that goes `0..SCREEN_HEIGHT-1`) is what feeds the lid lookup.

**X axis** — three accumulators, because the three X indices start at different offsets and (for left-eye `lidX`) advance in different directions:

| X source index | Start value (source-space)                     | Step per accumulator tick |
| -------------- | ---------------------------------------------- | ------------------------- |
| `scleraX`      | `scleraXsave` (gaze pan X)                     | +1                        |
| `irisX`        | `scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2`| +1                        |
| `lidX`         | `SCREEN_WIDTH - 1` (left) or `0` (right)       | -1 (left) / +1 (right)    |

All three X accumulators use the same numerator / denominator pair (`SCREEN_WIDTH` / `RENDER_WIDTH`), so they advance on the same render columns — only their start values and step signs differ. A single X Bresenham tick event can drive all three indices.

All iris / sclera pixel-decision logic (eyelid threshold test, polar iris lookup via `iScale`, sclera fallback) is unchanged.

### Address window

```c
display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);  // 0, 0, 466, 466
```

Replaces v1's `display_setAddrWindow(EYE_RENDER_X, EYE_RENDER_Y, 240, 240)`.

### Pixel buffer & DMA

Unchanged. `BUFFER_SIZE = 1024`, two ping-pong buffers, `dmaBuf` flip on each full flush. v2a pushes ~217 K pixels per frame in ~212 chunks + one tail flush.

### What `frame()`, `updateEye()`, `split()` look like

**Unchanged.** They operate entirely in source-space:

- `eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH - SCREEN_WIDTH)` — still 0..135 pan budget.
- `eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4` — ±4 source-px convergence.
- Eyelid tracking samples `upper[sampleY * SCREEN_WIDTH + sampleX]` — source indexing.

None of these files learn that `RENDER_*` exists.

### Invariants (reviewable)

1. When `SCREEN_WIDTH == RENDER_WIDTH` and `SCREEN_HEIGHT == RENDER_HEIGHT`, `drawEye()` produces bit-for-bit the same pixel stream as v1's `drawEye()`.
2. `drawEye()` references no render-only constant except `RENDER_WIDTH` / `RENDER_HEIGHT`.
3. `frame()`, `updateEye()`, `split()`, and `initEyes()` reference no `RENDER_*` constant.
4. For all valid inputs, every `pgm_read_*` inside `drawEye()` indexes within the declared array bounds (`scleraX < SCLERA_WIDTH`, `irisX < IRIS_WIDTH` before the iris-range check, `lidX ∈ [0, SCREEN_WIDTH)`).

## Configuration model (`config.h`)

```cpp
#pragma once

#include "data/default_large.h"   // defines SCREEN_WIDTH/HEIGHT,
                                  // SCLERA_*, IRIS_*, IRIS_MAP_* (source-space)

// Which eye this board renders.
#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_LEFT

// Physical panel = render target. Full-panel render in v2a.
#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466
#define RENDER_WIDTH  PANEL_WIDTH
#define RENDER_HEIGHT PANEL_HEIGHT

#define DISPLAY_BRIGHTNESS 200
#define TRACKING
#define AUTOBLINK
#define IRIS_SMOOTH

#if !defined(IRIS_MIN)
  #define IRIS_MIN 90
#endif
#if !defined(IRIS_MAX)
  #define IRIS_MAX 130
#endif
```

**Deleted from `config.h`:** `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`, `EYE_RENDER_X`, `EYE_RENDER_Y`.

## File changes

| File                                          | Change                                                                 |
| --------------------------------------------- | ---------------------------------------------------------------------- |
| `config.h`                                    | Drop `EYE_RENDER_*`, add `RENDER_WIDTH` / `RENDER_HEIGHT`.              |
| `eye_functions.ino`                           | Rewrite `drawEye()` scanline loops per this spec. `frame()` / `updateEye()` / `split()` unchanged. |
| `ESP32-uncanny-eyes-halloween-skull.ino`      | No change.                                                             |
| `display.ino`                                 | No change.                                                             |
| `data/default_large.h`                        | No change.                                                             |
| `README.md`                                   | Targeted edits: replace "240×240 centered on 466" wording with "full-panel 466×466 NN-stretched from the 240-baked asset." |
| `docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md` | New — this document. |

## Key decisions

1. **Nearest-neighbor, not bilinear.** Preserves the "one lookup per channel" inner-loop shape, avoids semantically-broken interpolation of the packed `{angle, distance}` polar iris map, lowest FPS risk, and reduces to identity under native-source assets. Bilinear remains possible as a future opt-in.
2. **Bresenham integer accumulator, not fixed-point.** Exact, no precision tuning, trivially collapses to identity, same code path for upscale / identity / downscale.
3. **Replace, not toggle.** Consistent with v1's stated "delete rather than `#ifdef`" philosophy. Fallback to v1 is a `git checkout` away.
4. **Source dims come from the asset header, render dims come from the panel.** The two are never spelled the same way in the same file. This is what makes `drawEye()` size-agnostic for future assets (120, 466-native, or anything else).
5. **Render-space entry points limited to `drawEye()` and `display_setAddrWindow()`.** All other files stay in source-space. Keeps the blast radius of render-dim changes tiny.
6. **FPS floor at 10, stretch at 20.** Aligns with NN's realistic ~3.77× per-pixel cost vs. v1. Separates "scaling correctness" from "renderer optimization" as independent concerns.

## Known risks / things to verify during v2a bring-up

1. **FPS under the 10 floor.** Most likely failure mode. *Verification:* serial `FPS=<n>` line, already present. *Mitigations in order:* (a) confirm Arduino-IDE build uses `-O2`/`-O3` (FQBN already set up correctly in the v1 plan); (b) row-base-pointer caching for `upper`/`lower`/`sclera` to drop per-pixel multiplies; (c) escalate to a dedicated perf follow-up spec rather than expanding this one.
2. **Bresenham off-by-one at the last render pixel.** The accumulator-resets-after-use form keeps `src_x ∈ [0, SRC)` for `rx ∈ [0, RENDER)`, but worth guarding. *Verification:* compile-time `static_assert(SCREEN_WIDTH <= RENDER_WIDTH)` and `static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT)` to lock the upscale-only assumption. Optional debug build adds runtime bounds assertions on each `pgm_read_*` index.
3. **Left-eye mirror Bresenham.** `lidX` decrements — if the accumulator drives it below 0, we index into flash that sits before `upper[]`. *Verification:* same `static_assert` coverage; manual flash-and-look of both `EYE_SIDE = LEFT` and `EYE_SIDE = RIGHT` builds.
4. **Visible NN stepping.** 240→466 ≈ 1.94× produces alternating "stretch by 2 / stretch by 1" rows and columns. May show mild stepping on iris edge and eyelid curve. *Verification:* visual inspection at intended viewing distance (~arm's length, inside a skull prop). If objectionable, fix is a future spec (native-466 assets or hybrid bilerp), not v2a scope.
5. **CO5300 col-offset at full width.** `display.ino` sets `CO5300_COL_OFFSET1 = 6`; v1 confirmed this is required to avoid a 6 px garbage strip on the left. `setAddrWindow(0, 0, 466, 466)` relies on Arduino_GFX applying that offset correctly at full width. *Verification:* boot and look — symptom (garbage strip at left) is immediately visible if broken.
6. **Frame-time variance under blink.** During a blink, most pixels short-circuit to `p = 0` — FPS briefly rises. Not a defect; noted so the FPS log is read correctly.

## Success criteria (v2a "done")

All of the following on the target board:

- Sketch builds from Arduino IDE with zero warnings beyond those present in v1.
- Boot is clean — no crash, brownout, or `gfx->begin() FAILED` log.
- Within ~2 s, the eye appears filling the full 466×466 round AMOLED. No centered inset.
- Eye motion, autoblink, eyelid tracking, and iris scaling behave the same as v1 — same timings, same easing, same randomness profile.
- `EYE_SIDE = LEFT` and `EYE_SIDE = RIGHT` builds both render correctly (eyelid mirror and convergence sign).
- Serial `FPS=<n>` reports ≥ 10 sustained. ≥ 20 is a stretch target, not a requirement.
- Code review: no surviving reference to `EYE_RENDER_X`, `EYE_RENDER_Y`, `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`, or "centered on 466×466" wording.
- Size-agnostic smoke test: dropping a differently-sized hypothetical asset header (same macro names, different values) into the `#include` compiles and runs without editing `drawEye()`.

## Out of scope — explicit non-work list

- Any interpolation other than NN.
- Regenerating `default_large.h` at 466-native.
- Dual-core rendering, DMA pipelining, IRAM hot-table relocation, or any perf work beyond what's needed for ≥ 10 FPS.
- Two-board sync, touch, IMU, mic/speaker, RTC, TF card.
- Battery / low-power, OTA, web/BLE config.

## Future work (sketch only, not committed)

- **v2a-followup: 466-native assets.** Regenerate `default_large.h` at source size 466 using the Adafruit art toolchain. Renderer needs no change — the Bresenham loop collapses to identity. Win is visual crispness.
- **v2a-perf: renderer optimization.** If ≥ 20 FPS becomes a goal: row-base-pointer caching, IRAM-resident hot tables, or split-core scanline / DMA pipelining.
- **v2b+ (unchanged from v1 spec):** second eye on a second board with ESP-NOW sync, touch-to-blink, IMU head-tracking, mic-triggered reactions. Each its own spec.
