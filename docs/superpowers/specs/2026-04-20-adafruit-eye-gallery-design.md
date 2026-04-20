# Adafruit eye gallery — compile-time first, touch cycling later

**Status:** phase 1 implemented; phase 2 (runtime serial + touch) implemented on `feat/adafruit-eye-gallery-phase2`  
**Date:** 2026-04-20  
**Depends on:** v2a row-expand renderer (source `SCREEN_*` → panel `RENDER_*` via NN stretch).  
**Out of scope here:** native-466 asset regeneration, two-board sync, OTA.

## Summary

Bring the full set of Adafruit Uncanny Eyes styles onto the Waveshare 466×466 AMOLED in two phases:

1. **Phase 1 — compile-time gallery:** Ship multiple `data/*.h` asset packs (PROGMEM, flat tables). Select exactly one eye per firmware build via a single `#include` in `config.h` (or a thin `eye_asset.h` wrapper). Rebuild and reflash to try the next style. No runtime switching, no duplicate symbols.

2. **Phase 2 — runtime gallery:** One firmware cycles through multiple styles on **CST9217 tap** (short press / release) or **serial** (`n` / newline). Implemented as **Option A:** prefixed PROGMEM tables + `EyeRuntime` active pointer (`tools/gen_eye_gallery_bundles.py`, SensorLib touch). Default partition ships **six** 128² classics (~94% flash); more styles need `huge_app` or LittleFS (Option B — not implemented).

## Goals (phase 1)

1. Every stock style the project cares about (default large + classic 128²-baked Adafruit eyes) **builds and runs** on the S3 AMOLED without changing `drawEyeRow()` / row-expand math.
2. **Document** which `#include` line selects which eye and any per-eye notes (e.g. owl: disable `TRACKING` in Adafruit docs).
3. Keep **flash size predictable**: one eye per binary; document approximate KiB per style if useful.
4. **No** `#include` of multiple full eye headers in one translation unit (avoids duplicate `sclera` / `iris` / … symbols).

## Non-goals (phase 1)

- Runtime eye switching.
- Regenerating art at 466² (optional follow-up after favorites are chosen).
- Dropping in raw `Uncanny_Eyes/graphics/*.h` **2D non-PROGMEM** tables — the S3 sketch expects **skull-style** flat `PROGMEM` headers matching `default_large.h` / ESP32 `data/*.h` layout.

## Asset sources

- **240² default:** existing `data/default_large.h`.
- **128² classic styles:** copy from a project that already ships **ESP32-compatible** headers (e.g. `ESP32LCDRound240x240Eyes/data/*.h` or equivalent), **or** run `Uncanny_Eyes/convert/tablegen.py` and post-process output to flat `PROGMEM` if that toolchain is preferred.

Verify each header defines the usual macros: `SCREEN_WIDTH` / `SCREEN_HEIGHT`, `SCLERA_*`, `IRIS_*`, `IRIS_MAP_*`, and the five tables the renderer expects (`sclera`, `iris`, `upper`, `lower`, `polar`).

## Configuration model (phase 1)

- `config.h` includes `data/eye_asset.h` only. **`data/eye_asset.h`** holds exactly one active `#include` for an eye (see README table).

- Optional: `README` or a short table in this doc listing style name ↔ filename ↔ approximate flash cost.

## Success criteria (phase 1)

- [x] Full classic set (`default_large.h` + ten `*Eye.h` from ESP32 `data/` pack) present; selection via `data/eye_asset.h`. `arduino-cli compile` verified for **`default_large`** and spot-check **`dragonEye`** (other styles same linkage pattern).
- [x] Each style fills the 466 panel per v2a when selected (unchanged renderer; `SCREEN_*` ≤ `RENDER_*`).
- [x] No async QSPI / renderer code changes for this feature; flash usage drops when using smaller 128² assets vs `default_large` (expected).

## Phase 2 (implemented — summary)

- Touch (CST9217) and **serial** increment the active style index; see [`README.md`](../../../README.md) and [`../plans/2026-04-20-adafruit-eye-gallery-phase2.md`](../plans/2026-04-20-adafruit-eye-gallery-phase2.md).
- **(A)** Renamed `const` tables + `EyeRuntime` — shipped. **(B)** LittleFS — only if flash budget requires it.

## References

- `docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md` — renderer boundary (source vs render).
- `Uncanny_Eyes/convert/tablegen.py` — regenerating or validating asset dimensions.
