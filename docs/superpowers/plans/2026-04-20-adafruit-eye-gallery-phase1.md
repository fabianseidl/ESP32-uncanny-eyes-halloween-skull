# Adafruit eye gallery (phase 1 — compile-time) — Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add all stock Adafruit Uncanny Eyes asset headers (PROGMEM, ESP32-compatible) under `data/`, and select exactly one style per firmware build via a single include file — no duplicate symbols, no renderer changes.

**Architecture:** Keep `drawEye()` / row expander / async QSPI untouched. Asset choice is **only** which `data/*.h` gets included. `SCREEN_WIDTH` / `SCREEN_HEIGHT` continue to size `line_src[]` in the main `.ino`; v2a `static_assert(SCREEN_* <= RENDER_*)` stays valid for 128² and 240² sources vs 466² panel. Phase 2 (touch cycling, LittleFS) is explicitly out of scope — see [`../specs/2026-04-20-adafruit-eye-gallery-design.md`](../specs/2026-04-20-adafruit-eye-gallery-design.md).

**Tech Stack:** Arduino + `arduino-cli`, ESP32-S3 (Waveshare 466×466 AMOLED), existing sketch layout (`config.h` → asset macros → `eye_functions.ino`).

**Verification model:** No host unit tests. Each task ends with `arduino-cli compile` using the FQBN from [`README.md`](../../../README.md). Optional: flash 2–3 styles and confirm full-panel eye + serial `FPS=` sane vs `default_large` baseline.

**Spec:** [`docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md`](../specs/2026-04-20-adafruit-eye-gallery-design.md)

**Branch (per [`AGENTS.md`](../../../AGENTS.md)):** `git checkout -b feat/adafruit-eye-gallery-phase1` before first commit.

---

## File structure

| File | Responsibility |
|------|----------------|
| `data/default_large.h` | **Keep.** 240²-baked default hazel (already in repo). |
| `data/*.h` (new) | Classic 128²-baked styles copied from a known-good ESP32 `data/` pack — flat `PROGMEM`, same symbol names as today (`sclera`, `iris`, `upper`, `lower`, `polar`). |
| `data/eye_asset.h` (new) | **Single active `#include`** for one eye; all other styles commented with short labels. Included **only** from `config.h` (replaces the direct `#include "data/default_large.h"`). |
| `config.h` | Replace `#include "data/default_large.h"` with `#include "data/eye_asset.h"`. Optionally move `QSPI_ASYNC_CHUNK_PX` + `EYE_SIDE` + panel/`RENDER_*` above the asset include if any header defines `IRIS_*` overrides (headers may define `IRIS_MIN`/`IRIS_MAX`; asset must be included before iris defaults in `config.h` — **keep current order:** chunk + **asset include** + side + panel + `RENDER_*` + `static_assert` + brightness + behavior flags + `IRIS_MIN`/`IRIS_MAX` fallbacks). |
| `README.md` | New subsection: eye gallery — how to switch style, table of files, FQBN one-liner, owl tracking note. |
| `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md` | Update **Status** to `approved` or `implemented` when phase 1 is done; tick success criteria checkboxes. |

**Do not copy:** `logo.h` (splash bitmap, not an eye — out of scope for this gallery).

**Source of copies (pick one that exists on the machine):**

- Sibling repo: `ESP32LCDRound240x240Eyes/data/*.h` (same machine layout as this plan was written against), **or**
- Re-clone [ESP32LCDRound240x240Eyes](https://github.com/) / Bodmer example if needed.

Expected filenames to add (10 classic + keep 1 large default — **11 distinct eye data files** in tree, **one linked per build**):

- `default_large.h` (existing)
- `catEye.h`, `defaultEye.h`, `doeEye.h`, `dragonEye.h`, `goatEye.h`, `naugaEye.h`, `newtEye.h`, `noScleraEye.h`, `owlEye.h`, `terminatorEye.h`

---

## Task 1: Import classic eye headers into `data/`

**Files:**

- Create: `data/catEye.h`, `data/defaultEye.h`, `data/doeEye.h`, `data/dragonEye.h`, `data/goatEye.h`, `data/naugaEye.h`, `data/newtEye.h`, `data/noScleraEye.h`, `data/owlEye.h`, `data/terminatorEye.h` (content copied from source pack)
- Unchanged: `data/default_large.h`

- [ ] **Step 1.1: Create branch**

```bash
cd /path/to/ESP32-uncanny-eyes-halloween-skull
git checkout -b feat/adafruit-eye-gallery-phase1
```

- [ ] **Step 1.2: Copy headers from the ESP32 TFT_eSPI example `data/` folder**

Adjust `SRC` to your local path (example: sibling checkout).

```bash
SRC="/path/to/ESP32LCDRound240x240Eyes/data"
DST="data"
for f in catEye defaultEye doeEye dragonEye goatEye naugaEye newtEye noScleraEye owlEye terminatorEye; do
  cp "${SRC}/${f}.h" "${DST}/${f}.h"
done
```

- [ ] **Step 1.3: Sanity-check one file**

Confirm top of `data/dragonEye.h` matches skull conventions: `#define SCLERA_*`, `PROGMEM`, `const uint16_t sclera[...]`, etc. (same shape as pre-port `default_large.h`).

- [ ] **Step 1.4: Commit**

```bash
git add data/*.h
git status   # expect 10 new files, not default_large.h unless touched
git commit -m "feat(data): add Adafruit classic eye headers for compile-time gallery"
```

---

## Task 2: Add `data/eye_asset.h` and wire `config.h`

**Files:**

- Create: `data/eye_asset.h`
- Modify: `config.h` (replace direct `default_large` include with `eye_asset.h`)

- [ ] **Step 2.1: Create `data/eye_asset.h`**

Only **one** of the lines below must be active (`#include` uncommented). Default stays `default_large.h` for zero behavior change until the user picks another style.

```cpp
// data/eye_asset.h -- Pick exactly ONE eye include. See README "Eye gallery".
// Paths are relative to the sketch root (Arduino resolves from project folder).
#pragma once

#include "data/default_large.h"

// Classic 128^2-baked (NN-stretched to 466^2 by the row expander):
// #include "data/defaultEye.h"
// #include "data/dragonEye.h"
// #include "data/noScleraEye.h"
// #include "data/goatEye.h"
// #include "data/newtEye.h"
// #include "data/terminatorEye.h"
// #include "data/catEye.h"
// #include "data/owlEye.h"
// #include "data/naugaEye.h"
// #include "data/doeEye.h"
```

- [ ] **Step 2.2: Edit `config.h`** — change line 12 from:

```cpp
#include "data/default_large.h"
```

to:

```cpp
#include "data/eye_asset.h"
```

Leave `QSPI_ASYNC_CHUNK_PX` and all defines below as-is (asset headers may define `IRIS_MIN`/`IRIS_MAX`; those apply when the corresponding include is active).

- [ ] **Step 2.3: Compile (default unchanged)**

From repo root (folder name must match main `.ino`):

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default"
arduino-cli compile --fqbn "$FQBN" .
```

**Expected:** `Success` / exit code 0. Visual: if flashed, same appearance as pre-change (still `default_large`).

- [ ] **Step 2.4: Commit**

```bash
git add data/eye_asset.h config.h
git commit -m "feat(config): eye_asset.h wrapper for compile-time style selection"
```

---

## Task 3: Spot-check alternate styles (compile-only matrix)

**Goal:** Prove each header compiles in isolation — catches missing files and macro clashes early.

- [ ] **Step 3.1: For each classic style, temporarily activate only that include**

Example for `dragonEye.h`: in `data/eye_asset.h`, comment `#include "data/default_large.h"`, uncomment `#include "data/dragonEye.h"`, run `arduino-cli compile` as in Task 2.3. Revert to `default_large` for the committed tree **or** leave the repo default as `default_large` after the loop.

- [ ] **Step 3.2: Document failures**

If any style fails (e.g. duplicate `IRIS_MIN` conflict — unlikely if one include only), note the error and fix (usually wrong double-include or wrong file).

**Optional shortcut:** a shell loop that swaps a `#define EYE_CHOICE` — **not required** for phase 1; manual edits are acceptable.

- [ ] **Step 3.3: Commit only if fixes were needed**

If no code changes after the loop, no extra commit.

---

## Task 4: README + spec status

**Files:**

- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md`

- [ ] **Step 4.1: Add README subsection (e.g. "Eye gallery (compile-time)")**

Content to include:

- Edit `data/eye_asset.h`: exactly one active `#include`.
- List: filename → short description (human default, dragon, cat, …).
- **Owl:** Adafruit docs recommend **disabling** `TRACKING` for `owlEye` — instruct to comment out `#define TRACKING` in `config.h` when using that asset, then restore for other eyes.
- Flash/PSRAM: one style per build; switching = edit + recompile + reflash.

- [ ] **Step 4.2: Update spec**

Set **Status** to `implemented` (or keep `draft` until merged). Check off phase 1 success criteria that apply.

- [ ] **Step 4.3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-04-20-adafruit-eye-gallery-design.md
git commit -m "docs: document compile-time eye gallery and mark spec phase 1"
```

---

## Task 5: Land the branch (optional, when ready)

- [ ] **Step 5.1: Merge to `main` per AGENTS.md** (`--no-ff`) after review, or open PR.

---

## Out of scope (do not implement in this plan)

- Touch or serial **runtime** style switching — separate plan after phase 1 is stable.
- Regenerating assets at 466² — optional follow-up.
- Including **multiple** full eye headers in one translation unit.
- Changing `Arduino_GFX`, `display_async`, or row-expand math for this feature.

---

## Rollback

- Revert commits on `feat/adafruit-eye-gallery-phase1`, or `git checkout main -- config.h data/` and delete added `data/*.h` / `eye_asset.h` if abandoning the branch.
