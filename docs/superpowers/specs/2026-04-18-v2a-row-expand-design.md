# v2a — Row-expand renderer: scale at pixel push (NN, size-agnostic)

**Status:** draft, pending user review
**Date:** 2026-04-18
**Supersedes:** [`2026-04-18-v2a-full-panel-render-design.md`](2026-04-18-v2a-full-panel-render-design.md) (Bresenham-in-drawEye variant). Replaces it as the chosen v2a approach.
**Scope:** v2a only — full-panel render on one board. Two-board sync, native-466 assets, touch/IMU/audio, and battery remain future work.

## Summary

Upgrade the renderer so the eye fills the entire 466×466 CO5300 AMOLED panel, using the existing 240-baked `data/default_large.h` asset stretched nearest-neighbor. **Upscale happens at the pixel-push stage, not inside the pixel-decision loop.** `drawEye()` still iterates the source grid (`SCREEN_HEIGHT × SCREEN_WIDTH`) and runs v1's per-pixel iris / sclera / eyelid logic once per source pixel, writing each result into a source-width line buffer. A dedicated **row expander** then horizontally NN-stretches that line into a render-width line via integer Bresenham and emits it to DMA, with vertical Bresenham deciding whether to re-emit the same expanded line for the next render row.

`drawEye()` is pure source-space. The expander is the only code that knows render-space exists. When `SCREEN_* == RENDER_*`, the expander collapses to `memcpy` + always-once emit — the renderer runs v1's pixel stream bit-for-bit.

v1's centered-240 render is removed. This spec replaces the superseded `v2a-full-panel-render-design.md` Bresenham-in-drawEye variant as v2a's chosen implementation.

## Goals

1. The eye fills the full 466×466 round AMOLED — no centered inset, no visible black border beyond the panel's own round mask.
2. Eye motion, autoblink, eyelid tracking, and iris scaling behave identically to v1 (same timings, easing, randomness profile).
3. Sustained FPS ≥ 10. Stretch target: ≥ 20. (This design makes the stretch target realistic — the expensive per-pixel work runs at source resolution, not render resolution.)
4. The renderer makes **no assumption about source asset dimensions.** Any valid `data/*.h` asset header (120, 240, 350, 466, …) compiles and runs without touching `drawEyeRow()`.
5. Left-eye and right-eye builds both render correctly — eyelid mirror direction and convergence-sign logic still work at source scale.
6. Source-space / render-space boundary is enforced by code layout: `drawEyeRow()` and the pixel-decision logic see only asset-header macros; the row expander sees only `RENDER_*` and `line_src` / `line_dst`. No file references both coordinate systems in the same function.

## Non-goals for v2a

- Bilinear or any non-NN interpolation.
- Regenerating eye assets at 466-native (future v2a-followup).
- Runtime-switchable asset tables — the design enables this cleanly (see Future work), but shipping it is a future spec.
- A `CENTERED_240` fallback toggle. v1 lives in git history; `#ifdef` fallback paths are not carried.
- Renderer perf work beyond what's needed to clear the 10 FPS floor.
- Two-board sync, touch / IMU / audio / RTC / TF, battery operation, OTA — all still v2b+.

## Target hardware

Unchanged from v1: one Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466×466). No new chip usage.

## Architecture

### Coordinate systems

The design enforces explicit separation of two coordinate systems that v1 conflated:

- **Source-space** — pixel dimensions baked into the asset header. `data/default_large.h` defines `SCREEN_WIDTH = 240`, `SCREEN_HEIGHT = 240`, `SCLERA_WIDTH = 375`, `IRIS_WIDTH = 180`, `IRIS_HEIGHT = 180`, `IRIS_MAP_WIDTH = 489`. Eye motion, eyelid tracking, convergence, iris lookups, and every per-pixel decision all live in source-space.
- **Render-space** — pixel dimensions of the panel draw window. `RENDER_WIDTH = RENDER_HEIGHT = 466`. Only the row expander (`expandRow` + `emitRow` + the vertical replicator in `drawEye`'s outer loop) and `display_setAddrWindow()` know about render-space.

The boundary is a line buffer: `drawEyeRow()` writes into `line_src[SCREEN_WIDTH]`, the expander reads from it. Nothing in `drawEyeRow()` references `RENDER_*`; nothing in the expander references iris / sclera / lid data.

### Where Bresenham lives

Two Bresenham accumulators, both owned by the row expander, both operating at render resolution:

- **Horizontal expander** — for each render column `rx ∈ [0, RENDER_WIDTH)`, derive a source column `sx ∈ [0, SCREEN_WIDTH)`:

  ```c
  int16_t sx = 0, hAccum = 0;
  for (int16_t rx = 0; rx < RENDER_WIDTH; rx++) {
    line_dst[rx] = line_src[sx];
    hAccum += SCREEN_WIDTH;
    while (hAccum >= RENDER_WIDTH) {
      hAccum -= RENDER_WIDTH;
      sx++;
    }
  }
  ```

  One load + one store per render pixel. No PROGMEM indirection, no branches beyond the accumulator tick.

- **Vertical replicator** — for each source row finished by `drawEyeRow()`, emit its expanded form 1 or more times so the total emitted render rows equals `RENDER_HEIGHT`:

  ```c
  int16_t vAccum = 0;
  for (int16_t sy = 0; sy < SCREEN_HEIGHT; sy++) {
    drawEyeRow(sy, line_src);
    expandRow(line_src, line_dst);
    emitRow(line_dst);                         // always emit at least once
    vAccum += RENDER_HEIGHT - SCREEN_HEIGHT;   // extra-emit credit
    while (vAccum >= SCREEN_HEIGHT) {
      vAccum -= SCREEN_HEIGHT;
      emitRow(line_dst);                       // extra emit (no recompute)
    }
  }
  ```

  Per-frame emit count: `SCREEN_HEIGHT` base emits + `(RENDER_HEIGHT - SCREEN_HEIGHT)` extra emits = `RENDER_HEIGHT` total. For 240→466: 240 base + 226 extra = 466. The principle is invariant: compute each source row exactly once, emit its expanded form 1 or more times. No source row is ever recomputed for vertical replication.

Both accumulators use exact integer arithmetic. Both collapse cleanly when `SCREEN_* == RENDER_*`: `hAccum` ticks once per step so `sx` advances by exactly 1 per render column (identity; `line_dst[i] == line_src[i]`); `vAccum += 0` per iteration so the inner `while` never fires (exactly one emit per source row).

### `drawEyeRow()` structure

`drawEyeRow(int16_t sy, uint16_t* line_src)` is v1's inner scanline, verbatim, with the pixel-write target changed from the DMA buffer to `line_src[sx]`. Per row it:

1. Derives `scleraY`, `irisY`, `screenY` from `sy` and the current gaze-pan Y (`scleraYsave`, `irisYsave`, same values v1 computed per row).
2. Initializes `scleraX`, `irisX`, `lidX` from gaze-pan X (same v1 starts).
3. Iterates `sx ∈ [0, SCREEN_WIDTH)`:
   - Advance `scleraX`, `irisX`, `lidX` by v1's per-pixel step (`lidX` decrements on left-eye builds, increments on right-eye, identical to v1).
   - Read `upper[screenY * SCREEN_WIDTH + lidX]` / `lower[...]`; lid threshold test.
   - If closed: `p = 0`. Else if iris in range: polar lookup via `iScale`; `p = iris[...]` or `p = sclera[...]`. Else: `p = sclera[scleraY * SCLERA_WIDTH + scleraX]`.
   - `line_src[sx] = p;`

No Bresenham in this function. No `RENDER_*` reference. No DMA touch.

The outer function (`drawEye()`) loops `sy ∈ [0, SCREEN_HEIGHT)`, calls `drawEyeRow(sy, line_src)`, then calls the expander + emitter, then advances the vertical accumulator.

### Address window

```c
display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);  // 0, 0, 466, 466
```

Set once at the start of `drawEye()`, before the source-row loop.

### Pixel buffer & DMA

DMA ping-pong unchanged from v1: `BUFFER_SIZE = 1024`, two buffers, `dmaBuf` flip on each full flush. `emitRow(line_dst)` pushes `RENDER_WIDTH` pixels into the active DMA buffer, flushing whenever it fills — same discipline v1 used for per-pixel writes, just fed from a `memcpy`-style line copy instead of one pixel at a time.

v2a pushes ~217 K pixels per frame in ~212 chunks + one tail flush. Exactly the same DMA shape as the superseded spec.

### Memory layout

| Buffer | Size | Purpose |
| --- | --- | --- |
| `line_src[SCREEN_WIDTH]` | 480 B (240 × 2) | Source row filled by `drawEyeRow()` |
| `line_dst[RENDER_WIDTH]` | 932 B (466 × 2) | Expanded row emitted to DMA |
| DMA ping-pong (2 × 1024 px) | 4 KB | Unchanged from v1 |

Total new RAM over v1: ~1.4 KB static.

### What `frame()`, `updateEye()`, `split()`, `initEyes()` look like

**Unchanged.** They operate entirely in source-space:

- `eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH - SCREEN_WIDTH)` — still 0..135 pan budget for the 240 asset.
- `eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4` — ±4 source-px convergence.
- Eyelid tracking samples `upper[sampleY * SCREEN_WIDTH + sampleX]` — source indexing.

None of these functions learn that `RENDER_*` exists.

### Invariants (reviewable)

1. When `SCREEN_WIDTH == RENDER_WIDTH` and `SCREEN_HEIGHT == RENDER_HEIGHT`, the pixel stream emitted by `drawEye()` is bit-for-bit identical to v1's. (`expandRow` = memcpy, vertical replication = always-once.)
2. `drawEyeRow()` references no render-space constant. Its only dim references are asset-header macros.
3. `expandRow()` references no pixel-decision logic. It reads `line_src`, writes `line_dst`, advances `hAccum`.
4. `frame()`, `updateEye()`, `split()`, `initEyes()` reference no `RENDER_*` constant.
5. For all valid inputs, every `pgm_read_*` inside `drawEyeRow()` indexes within declared array bounds (`scleraX < SCLERA_WIDTH`, `irisX < IRIS_WIDTH` before the iris-range check, `lidX ∈ [0, SCREEN_WIDTH)`).
6. **Upscale-only assumption**, locked by `static_assert(SCREEN_WIDTH <= RENDER_WIDTH)` and `static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT)`. The Bresenham math is correct for downscale, but the assertions prevent shipping a misconfigured downscale build unintentionally.

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

| File | Change |
| --- | --- |
| `config.h` | Drop `EYE_RENDER_*`, add `RENDER_WIDTH` / `RENDER_HEIGHT`. |
| `eye_functions.ino` | Restructure `drawEye()`: outer frame setup (address window, DMA reset), per-source-row loop that calls `drawEyeRow(sy, line_src)` then `expandRow(line_src, line_dst)` then `emitRow(line_dst)` (once or twice per vAccum state), tail DMA flush. Add `drawEyeRow()`, `expandRow()`, `emitRow()` as file-scope helpers. `frame()` / `updateEye()` / `split()` / `initEyes()` unchanged. |
| `ESP32-uncanny-eyes-halloween-skull.ino` | No change. |
| `display.ino` | No change. |
| `data/default_large.h` | No change. |
| `README.md` | Targeted edits: replace "240×240 centered on 466" wording with "full-panel 466×466 NN-stretched via row expander from the 240-baked asset." |
| `docs/superpowers/specs/2026-04-18-v2a-full-panel-render-design.md` | Add "Superseded by `2026-04-18-v2a-row-expand-design.md`" header at the top. Keep content intact for history. |
| `docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md` | New — this document. |
| `docs/superpowers/plans/2026-04-18-v2a-full-panel-render.md` | Rewrite to target this design (separate task via `writing-plans` after this spec is approved). |

## Key decisions

1. **Scale at pixel push, not inside pixel-decision loop.** Per-pixel iris / sclera / lid logic runs once per source pixel (`SCREEN_WIDTH × SCREEN_HEIGHT` = 57.6 K for the 240 asset) instead of once per render pixel (`RENDER_WIDTH × RENDER_HEIGHT` = 217 K). Cheap work (pixel replication, DMA fill) runs at render resolution. Right work at the right resolution.
2. **Line buffer is the source-space / render-space boundary.** A single 240-wide `line_src` array is the only thing that crosses the boundary. `drawEyeRow()` writes it; the expander reads it. Neither function references the other's coordinate system.
3. **Bresenham in the expander, not in `drawEyeRow()`.** Keeps `drawEyeRow()` readable as pure v1 source-space logic with the write target redirected. Keeps the expander readable as pure render-space scaling.
4. **Source-row buffer (approach A), not expanded-only (approach B) or double-buffered (C).** A preserves the clean source/render boundary at minimal RAM cost (480 B). B saves that RAM but puts horizontal Bresenham inside `drawEyeRow()`'s inner loop, breaking the boundary. C adds complexity only usable with DMA-overlapped or dual-core compute — v2a-perf territory.
5. **Nearest-neighbor, not bilinear.** Preserves the "one lookup per channel" inner-loop shape, avoids semantically-broken interpolation of the packed `{angle, distance}` polar iris map, lowest FPS risk. Bilinear remains possible as a future opt-in.
6. **Replace, not toggle.** Consistent with v1's stated "delete rather than `#ifdef`" philosophy. Fallback to v1 or the superseded Bresenham-in-drawEye variant is a `git checkout` / spec-revert away.
7. **Render-space entry points limited to the expander, `emitRow()`, and `display_setAddrWindow()`.** All other files stay in source-space. This is what makes `drawEyeRow()` size-agnostic for future assets (120, 466-native, or anything else) — and the runtime multi-asset future (see Future work) requires changing only `drawEyeRow()`'s loop bounds, not render-space code.
8. **FPS floor at 10, stretch at 20.** Same numbers as the superseded spec, but the stretch target is now realistic rather than aspirational. Separates "scaling correctness" from "renderer optimization" as independent concerns.

## Known risks / things to verify during v2a bring-up

1. **FPS under the 10 floor.** Lower risk than the superseded spec (~57.6 K expensive ops/frame vs. ~217 K), but still worth verifying. *Verification:* serial `FPS=<n>` line. *Mitigations in order:* (a) confirm `-O2`/`-O3` build flags; (b) inline `expandRow` and `emitRow` if the compiler does not; (c) row-base-pointer caching for `upper` / `lower` / `sclera` inside `drawEyeRow()`; (d) escalate to a dedicated perf follow-up spec.
2. **Vertical Bresenham off-by-one at the last source row.** Total emitted render rows must equal `RENDER_HEIGHT` exactly — no more, no less, or the address window undershoots / overshoots and DMA behavior is undefined. *Verification:* compile-time `static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT)`; debug-build runtime counter asserting emitted rows == `RENDER_HEIGHT` per frame.
3. **Horizontal Bresenham off-by-one at the last render pixel.** `sx` must stay in `[0, SCREEN_WIDTH)` for all `rx ∈ [0, RENDER_WIDTH)`. *Verification:* `static_assert(SCREEN_WIDTH <= RENDER_WIDTH)`; debug-build bounds check on `sx`.
4. **Left-eye mirror.** `lidX` starts at `SCREEN_WIDTH - 1` and decrements, fully inside `drawEyeRow()`. The expander never sees mirror state. *Verification:* flash and visually confirm both `EYE_SIDE = LEFT` and `EYE_SIDE = RIGHT` builds.
5. **Visible NN stepping.** 240→466 ≈ 1.94× produces alternating "stretch by 2 / stretch by 1" rows and columns. May show mild stepping on iris edge and eyelid curve. *Verification:* visual inspection at intended viewing distance (~arm's length, inside a skull prop). If objectionable, fix is a future spec (native-466 asset or hybrid bilinear), not v2a scope.
6. **CO5300 col-offset at full width.** `display.ino` sets `CO5300_COL_OFFSET1 = 6`; v1 confirmed this is required to avoid a 6 px garbage strip on the left. `setAddrWindow(0, 0, 466, 466)` relies on Arduino_GFX applying that offset correctly at full width. *Verification:* boot and look — symptom (garbage strip at left) is immediately visible if broken.
7. **Frame-time variance under blink.** During a blink, most pixels short-circuit to `p = 0` in `drawEyeRow()`, skipping PROGMEM reads — FPS briefly rises. Expander cost is constant regardless of eye state, so the blink FPS spike is smaller than in the superseded design. Not a defect; noted so the FPS log is read correctly.

## Success criteria (v2a "done")

All of the following on the target board:

- Sketch builds from Arduino IDE with zero warnings beyond those present in v1.
- Boot is clean — no crash, brownout, or `gfx->begin() FAILED` log.
- Within ~2 s, the eye appears filling the full 466×466 round AMOLED. No centered inset.
- Eye motion, autoblink, eyelid tracking, and iris scaling behave the same as v1 — same timings, same easing, same randomness profile.
- `EYE_SIDE = LEFT` and `EYE_SIDE = RIGHT` builds both render correctly (eyelid mirror and convergence sign).
- Serial `FPS=<n>` reports ≥ 10 sustained. ≥ 20 is a stretch target — realistically achievable with this design, but not a gate.
- Code review: no surviving reference to `EYE_RENDER_X`, `EYE_RENDER_Y`, `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`, or "centered on 466×466" wording. `drawEyeRow()` contains no `RENDER_*` reference. Expander contains no iris / sclera / lid logic.
- Size-agnostic smoke test: dropping a differently-sized hypothetical asset header (same macro names, different values — e.g. a 466-native `data/default_466.h`) into the `#include` compiles and runs without editing `drawEyeRow()` or the expander's algorithm. The expander auto-degenerates to `memcpy` + always-once emit when source and render dims match.

## Out of scope — explicit non-work list

- Any interpolation other than NN.
- Regenerating `default_large.h` at 466-native.
- Runtime-switchable asset tables (the architecture enables this cleanly; shipping it is a future spec).
- Dual-core rendering, DMA pipelining, IRAM hot-table relocation, or any perf work beyond what's needed for ≥ 10 FPS.
- Two-board sync, touch, IMU, mic/speaker, RTC, TF card.
- Battery / low-power, OTA, web/BLE config.

## Future work (sketch only, not committed)

- **v2a-followup: 466-native asset.** Regenerate `default_large.h` at source size 466 using the Adafruit art toolchain. Renderer needs no change — the expander collapses to `memcpy` + always-once emit. Win is visual crispness. `drawEyeRow()` does more expensive pixel decisions per frame (466² vs 240²), so FPS may compress; this design moves that from "impossible without rework" to "merely proportional."
- **v2a-perf: renderer optimization.** If ≥ 20 FPS becomes a goal and the raw design doesn't hit it: row-base-pointer caching inside `drawEyeRow()` to drop per-pixel multiplies, inline `expandRow` / `emitRow` if compiler misses, IRAM-resident hot tables, or DMA-overlapped row-compute (dual-buffer `line_src`).
- **Runtime-switchable asset tables.** Thread a `const EyeAsset*` through `drawEyeRow()`; its loop bounds and table pointers come from the struct instead of macros. Line buffers sized to `MAX_SCREEN_WIDTH`. The expander needs no change — it already sees only `line_src`, `line_dst`, `SCREEN_*`, `RENDER_*`, and `SCREEN_*` could become a per-frame variable read from the selected asset. This is the main architectural payoff vs. the superseded Bresenham-in-drawEye design: touch points concentrate in one function.
- **v2b+ (unchanged from v1 spec):** second eye on a second board with ESP-NOW sync, touch-to-blink, IMU head-tracking, mic-triggered reactions. Each its own spec.
