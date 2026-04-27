# Adafruit eye gallery (phase 2 — runtime touch / serial cycling) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one firmware that cycles through multiple baked eye styles at runtime: **short tap** on the panel (CST9217) or a **serial command** advances to the next style; all styles remain PROGMEM tables with **renamed symbols** plus a single active `EyeRuntime` descriptor so the renderer never includes two conflicting `sclera` / `#define SCREEN_WIDTH` translation units.

**Architecture:** Phase 1’s `data/eye_asset.h` single-include model is replaced (for gallery builds) by a **generated** translation unit `generated/eye_gallery_bundles.cpp` that holds every style’s tables under unique C identifiers (`eye_cat_sclera[]`, …). A small `EyeRuntime` struct holds dimensions (`screen_w`, `sclera_width`, `iris_map_width`, …), `iris_min` / `iris_max`, and `const` pointers to the five PROGMEM tables. `eye_functions.ino` stops using asset macros (`SCREEN_WIDTH`, `sclera`, …) in the hot path and reads the active bundle via `g_eye` (or explicit parameters). `line_src` is sized to **`EYE_GALLERY_MAX_SCREEN_W`** (compile-time max over the linked set, e.g. 240 when `default_large` is in the gallery). Touch uses the same **`TouchDrvCST92xx`** + I²C + `TP_RESET` / `TP_INT` pattern as Waveshare’s `06_LVGL_Widgets.ino` (see [`docs/hardware-notes.md`](../../../docs/hardware-notes.md)). **Option B (LittleFS + PSRAM)** is explicitly deferred unless flash measurement fails (see Task 1).

**Tech Stack:** Arduino + `arduino-cli`, ESP32-S3, `GFX Library for Arduino`, Waveshare touch stack (`TouchDrvCST92xx` / `TouchDrvCSTXXX.hpp` from the same SensorLib bundle as their demo), existing `display_async` + `eye_functions.ino` renderer.

**Verification:** No host unit tests. After each task: `arduino-cli compile` with the FQBN from [`README.md`](../../../README.md). After touch wiring: flash once and confirm tap advances style + serial `n` advances.

**Spec:** [`docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md`](../specs/2026-04-20-adafruit-eye-gallery-design.md) (Phase 2 section).

**Branch (per [`AGENTS.md`](../../../AGENTS.md)):** `git checkout -b feat/adafruit-eye-gallery-phase2` before the first commit.

---

## File structure

| File | Responsibility |
|------|----------------|
| `include/eye_runtime.h` (or sketch-root `eye_runtime.h`) | `struct EyeRuntime` — all per-asset dimensions + five `const` data pointers + `iris_min` / `iris_max` + short `id` string for Serial logging. |
| `eye_gallery.h` / `eye_gallery.cpp` | Build-time count `EYE_GALLERY_NUM`, `extern const EyeRuntime eye_gallery[]`, `void eye_gallery_init()`, `void eye_gallery_next()`, `void eye_gallery_poll()` (touch debounce + `Serial`), `const EyeRuntime* eye_gallery_active()`. |
| `generated/eye_gallery_bundles.cpp` | **Generated only.** Prefixed table definitions + `const EyeRuntime eye_gallery[] = { ... }`. Never hand-edit. |
| `tools/gen_eye_gallery_bundles.py` | Reads `data/*.h` list, regex-extracts `#define` dimensions and optional `IRIS_MIN` / `IRIS_MAX`, copies array bodies while renaming global symbols `sclera`→`eye_<slug>_sclera`, same for `iris`, `upper`, `lower`, `polar`; strips conflicting `#define` lines from the emitted copy. |
| `generated/eye_gallery_limits.h` | **Generated.** `constexpr unsigned EYE_GALLERY_MAX_SCREEN_W`, `_MAX_SCREEN_H`, and `static_assert` each style’s source ≤ `RENDER_*` (generator emits max + per-row comments). |
| `config.h` | For gallery firmware: `#include "generated/eye_gallery_limits.h"` then panel/`RENDER_*`; **remove** `#include "data/eye_asset.h"`. Add `#define EYE_GALLERY` (or inverse `#define EYE_COMPILETIME_ONLY`) to select build mode if you keep phase 1 as a supported path. |
| `ESP32-uncanny-eyes-halloween-skull.ino` | Declare `line_src[EYE_GALLERY_MAX_SCREEN_W]` (and keep `line_dst[RENDER_WIDTH]`). Call `eye_gallery_init()` after `display_begin()` (or after `Wire` + touch — see Task 6). |
| `eye_functions.ino` | Use active `EyeRuntime` for `drawEyeRow`, `expandRow`, `frame` (map, tracking sample, `drawEye`). |
| `display.ino` | Optionally export `Wire` init ordering vs touch — today `Wire.begin` is here; either call `touch_begin()` from `display_begin()` end or from `setup()` after `display_begin()`; document chosen order in `eye_gallery.cpp` header comment. |
| `README.md` | Phase 2: library install for touch, flash-size note, serial protocol, owl + `TRACKING` caveat when that style is in the rotation. |
| `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md` | Update Phase 2 from “pointer only” to `approved` / `implemented` when done. |

**Out of scope for this plan:** LittleFS packs (Option B) unless Task 1 forces a pivot — then stop and write a separate spec/plan for B only.

---

### Task 1: Branch + flash budget (decide A vs B)

**Files:**

- Modify: none yet
- Read: partition table / `arduino-cli compile -v` size section

- [ ] **Step 1.1: Create branch**

```bash
cd /Users/fabi/dev/ESP32-uncanny-eyes-halloween-skull
git checkout -b feat/adafruit-eye-gallery-phase2
```

- [ ] **Step 1.2: Decide linked set**

Default: all **ten** classic `*Eye.h` plus `default_large.h` (11 styles). If the size report shows **app partition overflow**, drop `default_large` from the generator list first (keeps `MAX_SCREEN_W == 128`), then re-measure. If still overflow, stop gallery A and open Option B plan (LittleFS).

- [ ] **Step 1.3: Note baseline size (optional but recommended)**

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default"
arduino-cli compile --fqbn "$FQBN" -v . 2>&1 | tail -n 30
```

Record **flash %** of the current single-asset build for comparison after bundles land.

- [ ] **Step 1.4: Commit**

Only if you added a short `docs/` note or script stub; otherwise no commit until Task 2 artifacts exist.

**Measured baseline (2026-04-20, `main` @ single asset via `data/eye_asset.h`, FQBN default 16M):** program **547122** bytes (**41%** of **1310720** max). Global vars **27668** bytes (**8%** of **327680**). Re-run after bundles land to compare.

---

### Task 2: `EyeRuntime` + generator skeleton

**Files:**

- Create: `eye_runtime.h`
- Create: `tools/gen_eye_gallery_bundles.py`
- Create: `generated/.gitkeep` (optional) and add `generated/*.cpp` / `generated/*.h` to `.gitignore` **or** commit generated files — pick one policy; this plan assumes **committed generated output** so CI/agents need not run Python.

**`eye_runtime.h` content (use verbatim):**

```cpp
#pragma once

#include <stdint.h>

struct EyeRuntime {
  const char* name;
  uint16_t screen_w, screen_h;
  uint16_t sclera_width, sclera_height;
  uint16_t iris_width, iris_height;
  uint16_t iris_map_width, iris_map_height;
  int16_t iris_min, iris_max;
  const uint16_t* sclera;
  const uint16_t* iris;
  const uint8_t* upper;
  const uint8_t* lower;
  const uint16_t* polar;
};
```

- [ ] **Step 2.1: Add Python generator (minimal behavior)**

Implement `tools/gen_eye_gallery_bundles.py` with:

1. A fixed ordered list `SPECS = [("cat", "data/catEye.h"), ...]` matching the styles you ship.
2. For each file, `open()` text, parse with regex, e.g. `r"#define\s+SCREEN_WIDTH\s+(\d+)"` → `screen_w`, same for `SCREEN_HEIGHT`, `SCLERA_WIDTH`, `SCLERA_HEIGHT`, `IRIS_WIDTH`, `IRIS_HEIGHT`, `IRIS_MAP_WIDTH`, `IRIS_MAP_HEIGHT`, and optional `IRIS_MIN` / `IRIS_MAX` (if missing, default 90 and 130 to match [`config.h`](../../../config.h) fallbacks).
3. Emit `generated/eye_gallery_limits.h`:

```cpp
#pragma once
constexpr unsigned EYE_GALLERY_MAX_SCREEN_W = 240;  // generator fills actual max
constexpr unsigned EYE_GALLERY_MAX_SCREEN_H = 240;
```

4. Emit `generated/eye_gallery_bundles.cpp` **header section** only in this step: `#include <Arduino.h>` + `#include "eye_runtime.h"` + forward declarations — **or** skip bodies until Task 3; minimum is limits + empty `extern const EyeRuntime eye_gallery[];` + `constexpr unsigned EYE_GALLERY_NUM = 0;` to compile.

- [ ] **Step 2.2: Run generator**

```bash
python3 tools/gen_eye_gallery_bundles.py
```

- [ ] **Step 2.3: Compile (sketch still on phase 1 includes)**

Expect success if you did not yet switch `config.h`.

- [ ] **Step 2.4: Commit**

```bash
git add eye_runtime.h tools/gen_eye_gallery_bundles.py generated/
git commit -m "feat(eye): EyeRuntime struct and gallery bundle generator scaffold"
```

---

### Task 3: Full generator output + prefixed tables

**Files:**

- Modify: `tools/gen_eye_gallery_bundles.py`
- Modify: `generated/eye_gallery_bundles.cpp` (via generator)
- Modify: `generated/eye_gallery_limits.h` (via generator)

- [ ] **Step 3.1: Extend generator to emit prefixed sources**

For each `(slug, path)`:

1. Copy the source text.
2. Remove lines matching `#define\s+IRIS_MIN` / `IRIS_MAX` (values captured into `EyeRuntime` initializer).
3. Replace global array names with unique identifiers:
   - `const uint16_t sclera[` → `const uint16_t eye_<slug>_sclera[`
   - `const uint16_t iris[` → `const uint16_t eye_<slug>_iris[`
   - `const uint8_t upper[` → `const uint8_t eye_<slug>_upper[`
   - `const uint8_t lower[` → `const uint8_t eye_<slug>_lower[`
   - `const uint16_t polar[` → `const uint16_t eye_<slug>_polar[`
4. Remove or rewrite `#define SCREEN_WIDTH` / `SCREEN_HEIGHT` / `SCLERA_*` / `IRIS_*` lines in the emitted fragment (they must **not** leak into the global preprocessor when all styles are concatenated in one `.cpp`).

5. Append one `EyeRuntime` initializer:

```cpp
  {
    .name = "cat",
    .screen_w = 128,
    .screen_h = 128,
    .sclera_width = /* from parse */,
    ...
    .iris_min = 90,
    .iris_max = 130,
    .sclera = eye_cat_sclera,
    .iris = eye_cat_iris,
    .upper = eye_cat_upper,
    .lower = eye_cat_lower,
    .polar = eye_cat_polar,
  },
```

6. Emit `const EyeRuntime eye_gallery[] = { ... };` and `constexpr unsigned EYE_GALLERY_NUM = sizeof(eye_gallery)/sizeof(eye_gallery[0]);`

- [ ] **Step 3.2: Regenerate and verify one style manually**

Diff `generated/eye_gallery_bundles.cpp` for `cat` only first; confirm `arduino-cli compile` after temporarily wiring a test `extern const EyeRuntime eye_gallery[]` include — full compile may wait until Task 4.

- [ ] **Step 3.3: Commit**

```bash
git add tools/gen_eye_gallery_bundles.py generated/
git commit -m "feat(eye): generate prefixed PROGMEM bundles and EyeRuntime table"
```

---

### Task 4: Refactor renderer to `EyeRuntime`

**Files:**

- Modify: `eye_functions.ino` (large)
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino`
- Modify: `config.h`
- Modify: `eye_gallery.h` (new) — declares `extern const EyeRuntime* g_eye_active;` set by gallery

**Approach:** Add `#include "eye_runtime.h"` and `#include "eye_gallery.h"` at top of `eye_functions.ino`. Introduce:

```cpp
static const EyeRuntime* g_eye = nullptr;

void eye_renderer_set_active(const EyeRuntime* e) {
  g_eye = e;
}
```

Change **every** use of `SCREEN_WIDTH`, `SCREEN_HEIGHT`, `SCLERA_WIDTH`, `SCLERA_HEIGHT`, `IRIS_WIDTH`, `IRIS_HEIGHT`, `IRIS_MAP_WIDTH`, `IRIS_MAP_HEIGHT`, `sclera`, `iris`, `upper`, `lower`, `polar` in `drawEyeRow`, `expandRow`, `frame`, and `split` to `g_eye->screen_w`, `g_eye->sclera`, etc. Add `assert(g_eye)` (or silent guard) in `frame` / `drawEye`.

**`expandRow` signature change:**

```cpp
static void expandRow(const uint16_t* src, uint16_t* dst, uint16_t screen_w) {
  uint16_t sx = 0;
  int32_t hAccum = 0;
  for (uint16_t rx = 0; rx < RENDER_WIDTH; rx++) {
    dst[rx] = src[sx];
    hAccum += screen_w;
    while (hAccum >= (int32_t)RENDER_WIDTH) {
      hAccum -= RENDER_WIDTH;
      sx++;
    }
  }
}
```

Call site: `expandRow(line_src, line_dst, g_eye->screen_w);`

**`updateEye`:**

```cpp
void updateEye(void) {
  newIris = random((int32_t)g_eye->iris_min, (int32_t)g_eye->iris_max);
  ...
}
```

**`split`:** replace `IRIS_MIN` / `IRIS_MAX` with `g_eye->iris_min` / `g_eye->iris_max`.

**`ESP32-uncanny-eyes-halloween-skull.ino`:**

```cpp
uint16_t line_src[EYE_GALLERY_MAX_SCREEN_W];
```

**`config.h`:** Remove `#include "data/eye_asset.h"`. Add:

```cpp
#include "generated/eye_gallery_limits.h"
static_assert(EYE_GALLERY_MAX_SCREEN_W <= RENDER_WIDTH,
              "max source width must fit row expander");
static_assert(EYE_GALLERY_MAX_SCREEN_H <= RENDER_HEIGHT,
              "max source height must fit vertical expander");
```

- [ ] **Step 4.1: Implement refactor + call `eye_renderer_set_active(&eye_gallery[0])` from `eye_gallery_init()`**

- [ ] **Step 4.2: Compile**

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default"
arduino-cli compile --fqbn "$FQBN" .
```

**Expected:** exit code 0. First style on screen matches index 0.

- [ ] **Step 4.3: Commit**

```bash
git add eye_functions.ino ESP32-uncanny-eyes-halloween-skull.ino config.h eye_gallery.cpp eye_gallery.h
git commit -m "refactor(eye): drive renderer from EyeRuntime for gallery builds"
```

---

### Task 5: Gallery state machine (index + serial)

**Files:**

- Create: `eye_gallery.cpp`
- Create: `eye_gallery.h`

**`eye_gallery.h`:**

```cpp
#pragma once
#include "eye_runtime.h"
#include <stddef.h>

void eye_gallery_init();
void eye_gallery_poll();  // call every loop: serial + debounced touch hook (Task 6)
void eye_gallery_next();
const EyeRuntime* eye_gallery_active();
```

**`eye_gallery.cpp` core:**

```cpp
#include "eye_gallery.h"

extern const EyeRuntime eye_gallery[];
extern const unsigned EYE_GALLERY_NUM;

static size_t s_idx = 0;

void eye_gallery_init() {
  s_idx = 0;
  eye_renderer_set_active(&eye_gallery[s_idx]);
  Serial.print("eye_gallery: init style ");
  Serial.println(eye_gallery[s_idx].name);
}

const EyeRuntime* eye_gallery_active() {
  return &eye_gallery[s_idx];
}

void eye_gallery_next() {
  s_idx = (s_idx + 1) % EYE_GALLERY_NUM;
  eye_renderer_set_active(&eye_gallery[s_idx]);
  Serial.print("eye_gallery: -> ");
  Serial.println(eye_gallery[s_idx].name);
}

void eye_gallery_poll() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N' || c == '\r' || c == '\n') {
      eye_gallery_next();
    }
  }
}
```

Declare `void eye_renderer_set_active(const EyeRuntime*);` in `eye_gallery.h` or a tiny `eye_renderer.h` included by both.

- [ ] **Step 5.1: Call `eye_gallery_init()` from `setup()`** after display is up and before the main loop relies on `g_eye`.

- [ ] **Step 5.2: Call `eye_gallery_poll()` at start of `loop()`**

- [ ] **Step 5.3: Compile + commit**

```bash
arduino-cli compile --fqbn "$FQBN" .
git add eye_gallery.cpp eye_gallery.h ESP32-uncanny-eyes-halloween-skull.ino
git commit -m "feat(eye): gallery index and serial next-style command"
```

---

### Task 6: CST9217 touch — tap to `eye_gallery_next()`

**Files:**

- Modify: `eye_gallery.cpp` (touch members + poll)
- Modify: `README.md` (dependency: SensorLib / same touch headers as Waveshare pack)

**Includes (match Waveshare `06_LVGL_Widgets.ino`):**

```cpp
#include "TouchDrvCSTXXX.hpp"
```

**Global in `eye_gallery.cpp`:**

```cpp
static TouchDrvCST92xx s_touch;
static volatile bool s_touch_irq = false;
static uint32_t s_last_advance_ms = 0;
```

**Init function `touch_gallery_begin()`** (call from `setup()` after `Wire.begin` — `display_begin()` already calls `Wire.begin(15, 14)` per [`display.ino`](../../../display.ino), so call touch init **after** `display_begin()`):

```cpp
void touch_gallery_begin() {
  pinMode(40, OUTPUT);  // TP_RESET — see docs/hardware-notes.md
  digitalWrite(40, LOW);
  delay(30);
  digitalWrite(40, HIGH);
  delay(50);

  s_touch.setPins(40, 11);  // TP_RESET, TP_INT
  bool ok = s_touch.begin(Wire, 0x5A, 15, 14);
  if (!ok) {
    Serial.println("eye_gallery: touch begin failed — serial 'n' only");
    return;
  }
  s_touch.setMaxCoordinates(466, 466);
  s_touch.setMirrorXY(true, true);
  attachInterrupt(11, []() { s_touch_irq = true; }, FALLING);
}
```

**In `eye_gallery_poll()` after Serial handling:**

```cpp
  if (s_touch_irq) {
    s_touch_irq = false;
    int16_t x[5], y[5];
    uint8_t n = s_touch.getPoint(x, y, s_touch.getSupportTouchPoint());
    uint32_t now = millis();
    if (n && (now - s_last_advance_ms) > 400) {
      s_last_advance_ms = now;
      eye_gallery_next();
    }
  }
```

Adjust debounce ms as needed. If the driver requires `digitalWrite(TP_RESET,...)` before `Wire.begin`, match Waveshare order exactly (their demo calls `Wire.begin` twice — follow the snippet in [`docs/hardware-notes.md`](../../../docs/hardware-notes.md) lines 217–234).

- [ ] **Step 6.1: Add library** per README (document ZIP path or `arduino-cli lib install` if the package name is known on your machine).

- [ ] **Step 6.2: Compile + flash + tap test**

- [ ] **Step 6.3: Commit**

```bash
git add eye_gallery.cpp README.md
git commit -m "feat(eye): CST9217 tap cycles gallery style"
```

---

### Task 7: Docs + spec status + optional phase 1 compatibility

**Files:**

- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md`

- [ ] **Step 7.1: README section “Eye gallery (runtime)”**

Document: `EYE_GALLERY` vs compile-time path (if you kept `eye_asset.h` behind `#ifdef`), serial `n`, touch debounce, flash size warning, **owl**: recommend disabling `#define TRACKING` in [`config.h`](../../../config.h) when `owlEye` is in the rotation (same as phase 1 README note).

- [ ] **Step 7.2: Spec** — set Phase 2 description to implemented approach (A), link this plan, update status date.

- [ ] **Step 7.3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md
git commit -m "docs: runtime eye gallery (phase 2) usage and spec status"
```

---

### Task 8: Merge / land (optional)

- [ ] **Step 8.1:** `git checkout main && git merge --no-ff feat/adafruit-eye-gallery-phase2` per AGENTS.md after review.

---

## Self-review (completed while authoring)

**1. Spec coverage:** Touch / GPIO-serial increment, Option A (renamed tables + active pointers), flash constraint called out with B as follow-up — covered. v2a “no renderer math change” is **not** literal here: the **same** NN expand math remains; only **macro → field** indirection changes. If you consider that a spec violation, document the exception in the design spec under Phase 2.

**2. Placeholder scan:** No `TBD` / empty test steps; compile commands are explicit.

**3. Type consistency:** `EyeRuntime` field names match the refactor described; `EYE_GALLERY_NUM` must be `constexpr unsigned` or `size_t` consistently in `.cpp` / `extern` decl.

---

## Rollback

`git checkout main` and delete branch, or revert merge commit. Restore `config.h` `#include "data/eye_asset.h"` and macro-based `eye_functions.ino` from `main` if abandoning phase 2.

---

Plan complete and saved to `docs/superpowers/plans/2026-04-20-adafruit-eye-gallery-phase2.md`.

**Execution options:**

1. **Subagent-driven (recommended)** — fresh subagent per task, review between tasks (`superpowers:subagent-driven-development`).
2. **Inline execution** — run tasks in this session with checkpoints (`superpowers:executing-plans`).

Which approach do you want to use?
