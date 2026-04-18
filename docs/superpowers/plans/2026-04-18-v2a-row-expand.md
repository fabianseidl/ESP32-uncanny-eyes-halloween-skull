# v2a Row-Expand Renderer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the renderer so the eye fills the full 466×466 CO5300 AMOLED, NN-stretched from the 240-baked `data/default_large.h` asset at the pixel-push stage — keeping per-pixel iris / sclera / eyelid logic at source resolution (57.6K ops/frame) while the row expander runs the cheap duplication at render resolution.

**Architecture:** `drawEye()` becomes an outer source-row loop. Per source row it calls `drawEyeRow(sy, line_src)` (v1's inner scanline with the pixel-write target redirected from the DMA buffer to a source-width line buffer), then `expandRow(line_src, line_dst)` (horizontal NN Bresenham, 240→466), then `emitRow(line_dst)` 1–2 times driven by vertical Bresenham. `drawEyeRow()` is pure source-space; the expander is the only code that knows `RENDER_*` exists.

**Tech Stack:** Arduino IDE + arduino-cli build, Arduino_GFX, ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.75, CO5300 QSPI 466×466), C++17, PROGMEM-resident eye assets.

**Verification model:** No host-side unit tests — this is embedded scanline code that depends on the panel + Arduino_GFX. Verification is (a) compile-clean builds, (b) flash-and-look, (c) serial `FPS=<n>` log.

**Spec:** [`docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md`](../specs/2026-04-18-v2a-row-expand-design.md)

---

## File Structure

Files created or modified, with responsibility:

- **`config.h`** — Render-target dims (`RENDER_WIDTH`, `RENDER_HEIGHT`), static_assert upscale-only guard. Already the home of `PANEL_*`, `EYE_SIDE`, etc. No new file needed.
- **`eye_functions.ino`** — Renderer surgery. Adds `line_src[SCREEN_WIDTH]` and `line_dst[RENDER_WIDTH]` static buffers. Splits `drawEye()` into `drawEye()` (outer frame setup + source-row loop + vertical replicator) + `drawEyeRow(sy, line_src)` (v1's inner scanline, pixel writes redirected) + `expandRow(line_src, line_dst)` (horizontal NN Bresenham) + `emitRow(line_dst)` (push RENDER_WIDTH pixels through the existing DMA ping-pong). `frame()`, `updateEye()`, `split()`, `initEyes()` unchanged.
- **`ESP32-uncanny-eyes-halloween-skull.ino`** — No change. Top-of-file comment touched up to reflect the new render scale.
- **`display.ino`** — No change. `display_setAddrWindow()` already takes arbitrary window dims.
- **`README.md`** — Targeted wording edits: "240×240 centered on 466" → "full-panel 466×466 NN-stretched via row expander from the 240-baked asset."

Task decomposition: three tasks. Each leaves the tree green and verifiable standalone.

- **Task 1** refactors the renderer into the new layout *without* changing behavior (still 240-centered). This is a pure-refactor commit — if it breaks, the break is isolated from the scaling logic.
- **Task 2** flips on the actual full-panel upscale: expand + replicate + full-panel address window + static_asserts. This is the user-visible change.
- **Task 3** runs through the verification matrix (both eye sides, FPS threshold, README wording).

---

## Task 1: Refactor `drawEye()` into row-expand shape (identity behavior)

Introduce the `drawEyeRow` / `expandRow` / `emitRow` decomposition with sizes still matching v1 — expander is identity, vertical replication is always-once, address window stays 240-centered. **Visual behavior is unchanged from v1 after this task.**

**Files:**
- Modify: `eye_functions.ino` (whole `drawEye()` body + add helpers)
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino` (add line buffers to the shared-buffers block)

- [ ] **Step 1.1: Add `line_src` and `line_dst` line buffers to the shared-buffers block.**

Edit `ESP32-uncanny-eyes-halloween-skull.ino`. After the `pbuffer` declaration, add:

```c
// Row-expand line buffers (see docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md).
// line_src holds one source row filled by drawEyeRow().
// line_dst holds the horizontally-expanded row pushed through emitRow().
// Sized at compile time from the asset header + config.h.
uint16_t line_src[SCREEN_WIDTH];
uint16_t line_dst[SCREEN_WIDTH];   // v2a Task 2 widens this to RENDER_WIDTH.
```

Why here, not in `eye_functions.ino`: `pbuffer` lives here as the canonical "shared render buffers" home. Keeping the line buffers next to it preserves the "all shared buffers in one place" convention. Task 2 resizes `line_dst` to `RENDER_WIDTH` — that edit is a one-liner.

- [ ] **Step 1.2: Add forward declarations for the new helpers to `eye_functions.ino`.**

Near the top of `eye_functions.ino`, before `initEyes()`, add:

```c
extern uint16_t line_src[];
extern uint16_t line_dst[];

static void drawEyeRow(uint32_t sy, uint32_t scleraXsave, uint32_t scleraY,
                       int32_t irisY, uint32_t iScale,
                       uint32_t uT, uint32_t lT);
static void expandRow(const uint16_t* src, uint16_t* dst);
static void emitRow(const uint16_t* dst);
```

`scleraXsave`, `scleraY`, `irisY` are passed so `drawEyeRow` can derive its per-row X starts and indices without touching `drawEye()`'s frame-level state — matches the "no shared globals between drawEye and drawEyeRow" implication of the spec's pure-source-space invariant.

- [ ] **Step 1.3: Replace `drawEye()` with the row-driven shape.**

Replace the entire body of `drawEye()` (currently lines 28–94 of `eye_functions.ino`) with:

```c
void drawEye( // Renders the eye. Inputs must be pre-clipped & valid.
    uint32_t iScale,   // Scale factor for iris
    uint32_t scleraX,  // First pixel X offset into sclera image
    uint32_t scleraY,  // First pixel Y offset into sclera image
    uint32_t uT,       // Upper eyelid threshold value
    uint32_t lT) {     // Lower eyelid threshold value
  display_startWrite();
  display_setAddrWindow(EYE_RENDER_X, EYE_RENDER_Y,
                        EYE_RENDER_WIDTH, EYE_RENDER_HEIGHT);

  const uint32_t scleraXsave = scleraX;
  int32_t        irisY       = (int32_t)scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  // Task 1: identity expander + always-once vertical emit. Task 2 wires in
  // horizontal Bresenham + vertical replication for full-panel render.
  for (uint32_t sy = 0; sy < SCREEN_HEIGHT; sy++) {
    drawEyeRow(sy, scleraXsave, scleraY + sy, irisY + (int32_t)sy,
               iScale, uT, lT);
    expandRow(line_src, line_dst);
    emitRow(line_dst);
  }

  display_endWrite();
}
```

Note the address window is still `EYE_RENDER_*`. The DMA-flush tail logic (the old `if (pixels) display_writePixels(...)` block) moves into `emitRow`.

- [ ] **Step 1.4: Add `drawEyeRow()` directly after `drawEye()`.**

```c
// v1's inner scanline, verbatim, with the pixel-write target changed from
// the DMA ping-pong buffer to line_src[sx]. Pure source-space: references
// only asset-header macros and the gaze-pan state passed in.
static void drawEyeRow(uint32_t sy, uint32_t scleraXsave, uint32_t scleraY,
                       int32_t irisY, uint32_t iScale,
                       uint32_t uT, uint32_t lT) {
  const uint16_t lidX_start = (EYE_SIDE == EYE_SIDE_LEFT) ? (SCREEN_WIDTH - 1) : 0;
  const int16_t  lidX_step  = (EYE_SIDE == EYE_SIDE_LEFT) ? -1 : 1;

  uint32_t scleraX = scleraXsave;
  int32_t  irisX   = (int32_t)scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
  uint16_t lidX    = lidX_start;
  uint32_t screenY = sy;

  for (uint32_t sx = 0; sx < SCREEN_WIDTH;
       sx++, scleraX++, irisX++, lidX += lidX_step) {
    uint32_t p, a, d;
    if ((pgm_read_byte(lower + screenY * SCREEN_WIDTH + lidX) <= lT) ||
        (pgm_read_byte(upper + screenY * SCREEN_WIDTH + lidX) <= uT)) {
      p = 0;
    } else if ((irisY < 0) || (irisY >= IRIS_HEIGHT) ||
               (irisX < 0) || (irisX >= IRIS_WIDTH)) {
      p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);
    } else {
      p = pgm_read_word(polar + irisY * IRIS_WIDTH + irisX);
      d = (iScale * (p & 0x7F)) / 128;
      if (d < IRIS_MAP_HEIGHT) {
        a = (IRIS_MAP_WIDTH * (p >> 7)) / 512;
        p = pgm_read_word(iris + d * IRIS_MAP_WIDTH + a);
      } else {
        p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);
      }
    }
    line_src[sx] = (uint16_t)p;
  }
}
```

The bit-level pixel decision is identical to v1 — only the write target changes.

- [ ] **Step 1.5: Add `expandRow()` (identity for Task 1).**

```c
// Horizontal NN stretch line_src[SCREEN_WIDTH] -> line_dst[RENDER_WIDTH]
// via integer Bresenham. Task 1: sizes match, so this collapses to a
// straight copy.
static void expandRow(const uint16_t* src, uint16_t* dst) {
  // Placeholder shape for Task 1: line_dst is currently sized SCREEN_WIDTH.
  // Task 2 rewrites this to the Bresenham form in the spec.
  for (uint16_t i = 0; i < SCREEN_WIDTH; i++) {
    dst[i] = src[i];
  }
}
```

- [ ] **Step 1.6: Add `emitRow()`.**

```c
// Push one expanded row through the DMA ping-pong. Flushes whenever the
// active buffer fills. Callers must wrap their sequence of emitRow() calls
// in display_startWrite() / display_endWrite(), and must invoke a final
// flush via emitRowFlushTail() before endWrite.
static void emitRow(const uint16_t* dst) {
  static uint32_t pixels = 0;
  // Task 1 emits SCREEN_WIDTH; Task 2 switches to RENDER_WIDTH.
  const uint32_t width = SCREEN_WIDTH;

  for (uint32_t i = 0; i < width; i++) {
    pbuffer[dmaBuf][pixels++] = dst[i];
    if (pixels >= BUFFER_SIZE) {
      yield();
      display_writePixels(&pbuffer[dmaBuf][0], pixels);
      dmaBuf = !dmaBuf;
      pixels = 0;
    }
  }
}

// Flush the partial DMA buffer at end of frame. Must be called between the
// last emitRow() and display_endWrite().
static void emitRowFlushTail() {
  static uint32_t* pixels_ref = nullptr;   // dummy - forces `pixels` to file scope.
  (void)pixels_ref;
}
```

That `emitRowFlushTail` stub is wrong — `pixels` is a function-local `static` and we can't flush it from outside. Fix: promote `pixels` to file scope so both functions can touch it.

Rewrite `emitRow` and add a real `emitRowFlushTail`:

```c
// Shared with emitRowFlushTail; promoted to file scope so the frame-end
// tail flush can drain the partial DMA buffer.
static uint32_t s_emitPixels = 0;

static void emitRow(const uint16_t* dst) {
  const uint32_t width = SCREEN_WIDTH;   // Task 2: RENDER_WIDTH
  for (uint32_t i = 0; i < width; i++) {
    pbuffer[dmaBuf][s_emitPixels++] = dst[i];
    if (s_emitPixels >= BUFFER_SIZE) {
      yield();
      display_writePixels(&pbuffer[dmaBuf][0], s_emitPixels);
      dmaBuf = !dmaBuf;
      s_emitPixels = 0;
    }
  }
}

static void emitRowFlushTail() {
  if (s_emitPixels) {
    display_writePixels(&pbuffer[dmaBuf][0], s_emitPixels);
    s_emitPixels = 0;
  }
}
```

- [ ] **Step 1.7: Call `emitRowFlushTail()` from `drawEye()`.**

Update `drawEye()` (body from Step 1.3) to call the tail flush before `display_endWrite()`:

```c
  for (uint32_t sy = 0; sy < SCREEN_HEIGHT; sy++) {
    drawEyeRow(sy, scleraXsave, scleraY + sy, irisY + (int32_t)sy,
               iScale, uT, lT);
    expandRow(line_src, line_dst);
    emitRow(line_dst);
  }

  emitRowFlushTail();
  display_endWrite();
```

Also add the forward declaration at the top of the file alongside the others from Step 1.2:

```c
static void emitRowFlushTail();
```

- [ ] **Step 1.8: Build.**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashMode=qio,PSRAM=opi,PartitionScheme=default_8MB ESP32-uncanny-eyes-halloween-skull.ino` (or the Arduino IDE "Verify" button).

Expected: clean compile, zero new warnings.

If it fails to compile with "cannot find `line_src` / `line_dst`": the forward `extern` declarations in `eye_functions.ino` must match the definitions in the main sketch. Verify Step 1.1 definitions are un-`static`.

- [ ] **Step 1.9: Flash and verify v1-identical behavior.**

Flash to the target board. Expected:
- Boot: "uncanny-eyes: boot" → "initEyes: single eye v1" → "uncanny-eyes: display_begin()" → "uncanny-eyes: running".
- Visual: 240×240 eye centered on 466×466 panel (same black border as v1).
- Eye motion, blink, iris scale: identical to v1 — same timing, easing, randomness.
- Serial `FPS=<n>` line: should report roughly the same FPS as pre-refactor v1 (indirect call overhead is ~one extra function boundary per row = negligible).

If the eye renders as garbage: most likely `s_emitPixels` isn't zeroed across calls. Check that `drawEye()` is the only caller of `emitRow()` and `emitRowFlushTail()`, and that every frame starts with `s_emitPixels == 0` (true by construction if every frame ends with `emitRowFlushTail()`).

- [ ] **Step 1.10: Commit.**

```bash
git add ESP32-uncanny-eyes-halloween-skull.ino eye_functions.ino
git commit -m "$(cat <<'EOF'
refactor(renderer): split drawEye into drawEyeRow + expandRow + emitRow

Prep for v2a row-expand full-panel render. No behavior change: expander
is identity, vertical replication is always-once, address window stays
at EYE_RENDER_* centered 240x240. drawEye now loops source rows, each
row computed by drawEyeRow into line_src, copied through expandRow into
line_dst, and pushed through emitRow into the existing DMA ping-pong.
Task 2 widens line_dst to RENDER_WIDTH and switches to Bresenham upscale.

See docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md.
EOF
)"
```

---

## Task 2: Full-panel 466×466 with horizontal + vertical Bresenham

Flip on the actual upscale. After this task, the eye fills the entire panel.

**Files:**
- Modify: `config.h` (remove `EYE_RENDER_*`, add `RENDER_*`, add `static_assert` guards)
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino` (resize `line_dst`)
- Modify: `eye_functions.ino` (Bresenham expander, vertical replicator, full-panel address window)
- Modify: `README.md` (wording)

- [ ] **Step 2.1: Rewrite `config.h`.**

Replace the "v1 renders the 240x240 eye natively, centered. No scaling." block with:

```c
// Physical panel = render target. Full-panel render in v2a.
#define RENDER_WIDTH  PANEL_WIDTH
#define RENDER_HEIGHT PANEL_HEIGHT

// Upscale-only guard. The Bresenham expander is correct under downscale
// too, but shipping a misconfigured downscale build is almost certainly
// unintentional -- lock it out.
static_assert(SCREEN_WIDTH  <= RENDER_WIDTH,
              "v2a assumes source asset width <= render/panel width");
static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT,
              "v2a assumes source asset height <= render/panel height");
```

Remove the `EYE_RENDER_WIDTH`, `EYE_RENDER_HEIGHT`, `EYE_RENDER_X`, `EYE_RENDER_Y` defines entirely.

The result of the full file should be:

```c
// v2a config for Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466x466).
// Per-board settings. Flash one copy of this firmware to each board with the
// correct EYE_SIDE.

#pragma once

#include "data/default_large.h"

#define EYE_SIDE_LEFT  0
#define EYE_SIDE_RIGHT 1
#define EYE_SIDE       EYE_SIDE_LEFT

#define PANEL_WIDTH   466
#define PANEL_HEIGHT  466

#define RENDER_WIDTH  PANEL_WIDTH
#define RENDER_HEIGHT PANEL_HEIGHT

static_assert(SCREEN_WIDTH  <= RENDER_WIDTH,
              "v2a assumes source asset width <= render/panel width");
static_assert(SCREEN_HEIGHT <= RENDER_HEIGHT,
              "v2a assumes source asset height <= render/panel height");

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

- [ ] **Step 2.2: Resize `line_dst` in the main sketch.**

In `ESP32-uncanny-eyes-halloween-skull.ino`, change:

```c
uint16_t line_dst[SCREEN_WIDTH];   // v2a Task 2 widens this to RENDER_WIDTH.
```

to:

```c
uint16_t line_dst[RENDER_WIDTH];
```

Update the comment above the buffers to reference the row-expand design spec as the source of truth.

- [ ] **Step 2.3: Rewrite `expandRow()` to horizontal Bresenham.**

Replace the identity body from Task 1 Step 1.5 with:

```c
// Horizontal NN stretch src[SCREEN_WIDTH] -> dst[RENDER_WIDTH] via
// integer Bresenham. Collapses to a pixel-for-pixel copy when
// SCREEN_WIDTH == RENDER_WIDTH.
static void expandRow(const uint16_t* src, uint16_t* dst) {
  uint16_t sx     = 0;
  int32_t  hAccum = 0;
  for (uint16_t rx = 0; rx < RENDER_WIDTH; rx++) {
    dst[rx] = src[sx];
    hAccum += SCREEN_WIDTH;
    while (hAccum >= (int32_t)RENDER_WIDTH) {
      hAccum -= RENDER_WIDTH;
      sx++;
    }
  }
}
```

Note on types: `hAccum` as `int32_t` headroom-proofs against overflow for any conceivable SRC/RENDER combination (both < 65536 keeps `hAccum + SCREEN_WIDTH` well within `int32_t`). The spec's upscale-only static_assert means `hAccum` stays non-negative, but int32_t is cheap insurance.

- [ ] **Step 2.4: Widen `emitRow()` to push `RENDER_WIDTH` pixels.**

In `eye_functions.ino`, change the local `width` in `emitRow()` from `SCREEN_WIDTH` to `RENDER_WIDTH`:

```c
static void emitRow(const uint16_t* dst) {
  for (uint32_t i = 0; i < RENDER_WIDTH; i++) {
    pbuffer[dmaBuf][s_emitPixels++] = dst[i];
    if (s_emitPixels >= BUFFER_SIZE) {
      yield();
      display_writePixels(&pbuffer[dmaBuf][0], s_emitPixels);
      dmaBuf = !dmaBuf;
      s_emitPixels = 0;
    }
  }
}
```

- [ ] **Step 2.5: Switch `drawEye()` to full-panel address window + vertical replicator.**

Replace the `drawEye()` body (from Task 1 Step 1.3 / 1.7) with:

```c
void drawEye(uint32_t iScale, uint32_t scleraX, uint32_t scleraY,
             uint32_t uT, uint32_t lT) {
  display_startWrite();
  display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

  const uint32_t scleraXsave = scleraX;
  int32_t        irisY       = (int32_t)scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  int32_t vAccum = 0;
  for (uint32_t sy = 0; sy < SCREEN_HEIGHT; sy++) {
    drawEyeRow(sy, scleraXsave, scleraY + sy, irisY + (int32_t)sy,
               iScale, uT, lT);
    expandRow(line_src, line_dst);
    emitRow(line_dst);                     // always at least once
    vAccum += (int32_t)RENDER_HEIGHT - (int32_t)SCREEN_HEIGHT;
    while (vAccum >= (int32_t)SCREEN_HEIGHT) {
      vAccum -= SCREEN_HEIGHT;
      emitRow(line_dst);                   // extra emit (no recompute)
    }
  }

  emitRowFlushTail();
  display_endWrite();
}
```

Invariant: total `emitRow` calls per frame = `SCREEN_HEIGHT + (RENDER_HEIGHT - SCREEN_HEIGHT)` = `RENDER_HEIGHT`. For 240→466: 240 + 226 = 466. ✓

- [ ] **Step 2.6: Build.**

Run: `arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashMode=qio,PSRAM=opi,PartitionScheme=default_8MB ESP32-uncanny-eyes-halloween-skull.ino`

Expected: clean compile, zero new warnings. The two `static_assert`s must pass with the 240 asset + 466 render dims.

If `static_assert` fires: someone changed `PANEL_*` or swapped in a different asset. Fix by restoring upscale-compatible dims.

- [ ] **Step 2.7: Flash and verify full-panel render.**

Flash. Expected visuals:
- Eye fills the entire 466×466 round AMOLED. No centered inset, no black gutter beyond the panel's own round-mask.
- Eye motion, autoblink, eyelid tracking, iris scaling: timing identical to v1 (same `frame()` / `updateEye()` / `split()`).
- Serial `FPS=<n>` line: **≥ 10** sustained. ≥ 20 is the stretch target — record the actual number.
- No garbage strip at the left edge (if you see one, the `CO5300_COL_OFFSET1 = 6` in `display.ino` isn't being applied at this address-window size — spec risk #6).

If the eye renders stretched but wrong-aspect: check that both `static_assert`s passed (dims agree). If vertical NN stepping is obviously wrong (one band of the eye duplicated way too many times): re-verify Step 2.5's vAccum formula against the spec's "240 base + 226 extras = 466" invariant.

- [ ] **Step 2.8: Update `README.md`.**

Find and replace references to the v1 centered-240 behavior. Specifically:
- Any text mentioning "240×240 centered on 466" → "full-panel 466×466 NN-stretched from the 240-baked asset via a row expander"
- Any text saying "EYE_RENDER_*" as a config knob → removed (those constants no longer exist)

If the README already contained a "v1 behavior" section, rename it "v2a behavior" and update accordingly.

Run `grep -n "240×240\|EYE_RENDER\|centered" README.md` to find remaining stale references.

- [ ] **Step 2.9: Update the top-of-file comment in the main sketch.**

In `ESP32-uncanny-eyes-halloween-skull.ino`, change:

```c
// Renders one eye (EYE_SIDE in config.h) as a 240x240 image centered on
// the 466x466 CO5300 AMOLED. See docs/superpowers/specs for the design.
```

to:

```c
// Renders one eye (EYE_SIDE in config.h) full-panel on the 466x466 CO5300
// AMOLED, NN-stretched from the 240-baked asset via a row expander. See
// docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md.
```

- [ ] **Step 2.10: Commit.**

```bash
git add config.h ESP32-uncanny-eyes-halloween-skull.ino eye_functions.ino README.md
git commit -m "$(cat <<'EOF'
feat(renderer): v2a full-panel 466x466 NN upscale via row expander

Scale happens at the pixel-push stage: drawEyeRow writes one source row
(240 px) into line_src, expandRow horizontally NN-stretches to line_dst
(466 px) via integer Bresenham, and emitRow pushes it through the DMA
ping-pong 1 or 2 times based on vertical Bresenham. Total emits per
frame = RENDER_HEIGHT exactly. Address window is now full-panel.

Per-pixel iris/sclera/eyelid logic runs at source resolution
(57.6K ops/frame), independent of render resolution.

See docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md.
EOF
)"
```

---

## Task 3: Verification matrix

Exercise both eye-side builds and record the FPS figure for the commit log.

**Files:**
- Modify: `config.h` (flip `EYE_SIDE` for the left/right check; revert after)

- [ ] **Step 3.1: Build + flash `EYE_SIDE = EYE_SIDE_RIGHT`.**

Edit `config.h`: change `#define EYE_SIDE EYE_SIDE_LEFT` to `#define EYE_SIDE EYE_SIDE_RIGHT`.

Build + flash. Expected: eye renders full-panel; eyelid mirror direction is flipped relative to the LEFT build (caruncle on the opposite side); iris convergence offset has flipped sign (`+4` instead of `-4` in the source-space gaze code).

If the eye renders but looks structurally wrong (eyelid corner on the wrong side): the mirror logic in `drawEyeRow()` uses the same `lidX_start`/`lidX_step` as v1, so if v1's LEFT and RIGHT both worked, v2a's should too. If they didn't — surface this now as a pre-existing bug, not a v2a regression.

- [ ] **Step 3.2: Revert `EYE_SIDE` to `EYE_SIDE_LEFT` (project default) and re-flash.**

Edit `config.h` back to `#define EYE_SIDE EYE_SIDE_LEFT`. Build + flash + verify.

Do not commit this flip-and-revert — it's a verification maneuver, not a change.

- [ ] **Step 3.3: Read sustained FPS from serial.**

With the left-eye build running, open the serial monitor at 115200 baud. Let the sketch run for at least 30 seconds so the `FPS=<n>` line (emitted every 256 frames) reports a stable value. Record the number.

Success criteria from the spec:
- **Gate:** ≥ 10 sustained.
- **Stretch (nice-to-have):** ≥ 20.

If < 10: apply mitigations from spec risk #1 in order:
1. Verify build is using `-O2` or `-O3`. For arduino-cli, this is the default for release builds.
2. Consider `__attribute__((always_inline))` on `expandRow` and `emitRow`.
3. Cache row-base pointers (`upper + screenY*SCREEN_WIDTH` etc.) at the top of `drawEyeRow()` to drop per-pixel multiplies.

If any of (1)–(3) is needed, capture as a follow-up commit or escalate to a perf spec.

- [ ] **Step 3.4: Final code-review pass.**

Run these greps to confirm the spec's "no surviving references" criterion:

```bash
grep -rn "EYE_RENDER" . --include="*.ino" --include="*.h" --include="*.md"
grep -rn "centered on 466" . --include="*.ino" --include="*.h" --include="*.md"
grep -n "RENDER_" eye_functions.ino | grep -v "^[[:space:]]*//"
```

Expected:
- First two: no matches in source files (docs/plans in `docs/superpowers/` may still mention them historically; that's fine).
- Third: only matches in `drawEye()` itself (address window set) — never inside `drawEyeRow`, `expandRow`'s core logic beyond the Bresenham denominator, or pixel-decision code.

If `drawEyeRow` references `RENDER_*`: invariant #2 in the spec is violated — fix by moving the reference into `drawEye()` or the expander.

- [ ] **Step 3.5: Record the FPS number in the success-criteria log.**

Add a one-liner to the spec's "Success criteria" section (or append below it) noting the measured FPS:

```markdown
**Measured FPS (v2a left-eye, default_large.h, 240→466 NN, commit <hash>):** <n> fps.
```

This is for future perf-regression reference. Commit just the spec edit.

```bash
git add docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md
git commit -m "docs: record v2a baseline FPS measurement"
```

---

## Self-review

**Spec coverage:**
- Coordinate-system separation → Task 1 Step 1.4 (`drawEyeRow` pure source-space), Task 2 Steps 2.3/2.5 (`expandRow`/`drawEye` hold render-space). ✓
- Bresenham correctness (horizontal + vertical) → Step 2.3 + 2.5; invariant trace in the task body. ✓
- Memory layout (line_src, line_dst, DMA ping-pong) → Steps 1.1 + 2.2. ✓
- static_assert upscale-only guard → Step 2.1. ✓
- Address window 0,0,RENDER,RENDER → Step 2.5. ✓
- README wording + top-of-file sketch comment → Steps 2.8 + 2.9. ✓
- Left/right eye mirror verification → Steps 3.1–3.2. ✓
- FPS ≥ 10 verification → Step 3.3. ✓
- No surviving EYE_RENDER_* / "centered on 466×466" references → Step 3.4. ✓
- Identity collapse (when SRC == RENDER) → Task 1 is the literal identity case, verified in Step 1.9. Task 2's Bresenham collapses to identity per the spec's math — trivially verified by a compile-only check with a hypothetical SRC=RENDER asset (not exercised on hardware in this plan; documented in spec Success Criteria as a smoke test).
- Runtime-switchable asset tables → spec Future Work; out of scope here. ✓
- Bilinear / native-466 assets / perf optimization → spec Future Work; out of scope here. ✓

**Placeholder scan:** No "TBD", no "add appropriate error handling", no "similar to Task N", no "fill in details". Every step has exact code or exact command.

**Type consistency:**
- `drawEyeRow` signature — (uint32_t sy, uint32_t scleraXsave, uint32_t scleraY, int32_t irisY, uint32_t iScale, uint32_t uT, uint32_t lT) — matches between forward declaration (Step 1.2) and definition (Step 1.4), and the call site in Step 1.3 / 2.5.
- `expandRow(const uint16_t* src, uint16_t* dst)` — consistent Steps 1.2 / 1.5 / 2.3.
- `emitRow(const uint16_t* dst)` — consistent Steps 1.2 / 1.6 / 2.4.
- `line_src` / `line_dst` — declared `uint16_t[SCREEN_WIDTH]` / `uint16_t[RENDER_WIDTH]` in the main sketch (Steps 1.1 / 2.2), `extern uint16_t line_src[]` / `line_dst[]` in `eye_functions.ino` (Step 1.2).
- `s_emitPixels` — file-scope `static uint32_t` in `eye_functions.ino` (Step 1.6), referenced by both `emitRow` and `emitRowFlushTail` (Step 1.6 / 1.7).

All consistent.
