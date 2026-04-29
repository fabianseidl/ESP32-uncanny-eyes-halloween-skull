# Eye Sync — Phase B: Animation Lockstep (Deterministic + Leader Pulse)

**Status:** implemented (`feat/eye-sync-phase-b`, 2026-04-29)  
**Date:** 2026-04-29  
**Depends on:** Phase C — [`2026-04-28-eye-sync-phase-c-design.md`](2026-04-28-eye-sync-phase-c-design.md) (gallery index over ESP-NOW)

## Summary

Phase C keeps both boards on the **same gallery style**. Phase B makes motion feel **alive together**: correlated gaze, blinks, and iris behavior, without per-frame radio traffic.

**Approach:** Shared **animation seed** plus a **low-rate leader “pulse”** (~10 Hz) so random decisions advance in **lockstep** across boards. Pure shared-`randomSeed()` is **insufficient** because `frame()` consumes the global PRNG a **frame-count-dependent** number of times inside `split()`’s real-time loop; boards with different FPS diverge even with identical firmware.

## Goals and Non-Goals

**Goals**

- Gaze targets, autoblink timing, and iris recursion (`split` / `updateEye`) stay **correlated** between the two boards for extended runs.
- **Low airtime:** default **~10 Hz** unicast or broadcast pulse plus existing Phase C gallery traffic; no per-scanline or per-physical-frame packets.
- Same firmware image on both boards; eye side continues to resolve from STA MAC (see [`config.h`](../../../config.h) `EYE_SIDE_MAC_*`).
- `EYE_SYNC_ENABLE 0` remains a clean single-eye build with no WiFi/ESP-NOW linkage.
- Hot path unchanged: sync work at **frame boundaries** / RX drain only, not inside `drawEyeRow` / QSPI queue.

**Non-Goals**

- Cryptographic authentication or encryption.
- More than two coordinated boards.
- Deterministic resolution of simultaneous gallery taps in the same millisecond (Phase C semantics stand).
- Perfect sub-millisecond visual alignment of raster output (radio jitter remains).

## Technical Constraint (Why Seed Alone Fails)

In [`eye_functions.cpp`](../../../eye_functions.cpp):

1. **`updateEye()`** calls `split(oldIris, newIris, …)` with **`random()`** used in the **recursive** phase of `split()` (no `frame()` calls yet).
2. At the **leaf** of `split()`, a loop runs **`frame(v)`** until `micros()` advances past `duration`.
3. **`frame()`** calls **`random()`** for idle gaze timing, new `(eyeNewX, eyeNewY)`, and autoblink intervals.

The number of **`frame()`** iterations for a given wall-time interval **depends on render duration per frame** (FPS). Slower boards execute **fewer** `frame()` calls and consume **fewer** PRNG outputs than faster boards, so **identical `randomSeed()`** still produces **divergent** streams over time.

**Conclusion:** Gaze/autoblink randomness must **not** be tied to “once per physical frame” via the global PRNG. **Either** advance a dedicated anim PRNG **once per shared logical step** (leader pulse), **or** accept visible drift (explicitly weaker tier, not the default in this spec).

## Architecture

### Leader

- The **left-eye board** (`g_eye_side == EYE_SIDE_LEFT` after [`eye_side_init()`](../../../eye_side.cpp); STA MAC matches `EYE_SIDE_MAC_LEFT` in [`config.h`](../../../config.h)) is the **animation leader**.
- The right-eye board is the follower. If MAC-based detection fails and `EYE_SIDE_MAC_FALLBACK` applies on both units, behavior is undefined; operators must ensure distinct MACs per board (already required for Phase C sanity).

Optional compile-time override MAY be added later (e.g. `EYE_SYNC_ANIM_FORCE_LEADER 1` on exactly one board); this spec recommends MAC-based leader only to preserve **one binary** for both eyes.

### Two stochastic domains

1. **Iris / `split` recursion** — Uses Arduino **`random()`** after a **`randomSeed()`** applied whenever the **animation epoch** advances (gallery change and/or explicit seed in pulse).
2. **Gaze + autoblink** — Uses a **file-scoped `uint32_t` (or PCG) anim PRNG**, advanced **exactly once per leader pulse** (when applying a new `step`), **never** once per physical `frame()` from the global `random()`.

Interpolation (easing, blink envelope, lid thresholds) continues every physical `frame()` using **`micros()`**; only **discrete random choices** move to the pulse-driven path.

### Leader pulse

- Leader sends **`TYPE_ANIM_PULSE`** at a fixed interval (default **100 ms → 10 Hz**), defined in `config.h` as `EYE_SYNC_ANIM_PULSE_MS`.
- Each message carries a monotonically increasing **`step`** (e.g. `uint32_t`; on overflow, wrap with documented compare logic).
- Follower drains pulses in `eye_sync_tick()` (same RX ring pattern as Phase C). On each accepted pulse: if `step` is next in sequence, advance anim PRNG once and run the small FSM that today uses `random()` inside `frame()` for **new targets / blink schedule**; then interpolate until the next pulse.
- **Out-of-order / gap:** If `step > expected + 1`, follower **fast-forwards** the anim PRNG by `(step - expected)` steps (or applies a single compound draw — implementation choice with identical outcome) to **resync** after packet loss.

### Gallery change and epoch reset

On **local** gallery advance (touch/serial): existing Phase C broadcasts gallery index. Phase B adds:

- Leader generates a new **uint32_t anim_seed** (e.g. `esp_random()` mixed with MAC nibble).
- Leader broadcasts **`TYPE_ANIM_SEED`** (or combined gallery+seed policy — see Wire format) so follower applies `randomSeed(anim_seed)`, resets anim PRNG from `anim_seed`, resets `step` to 0 (or agreed baseline), and resets gaze/blink **state variables** to a defined idle (e.g. centered eye, `eyeInMotion == false`, autoblink timers restarted from pulse-driven draws).

Race handling reuses Phase C **race-guard** for gallery; anim seed application should obey the same “local tap wins” window so a stale pulse does not overwrite a fresh local transition.

### Left / right mirror

Logical gaze remains in shared **0…1023** space. Existing `EYE_SIDE` draw-time mirror in `drawEyeRow` and sclera offsets remains; no separate wire format for “mirrored” coordinates.

## Wire Format

Phase C uses a fixed **8-byte** [`EyeSyncMsg`](../../../eye_sync.h). Phase B introduces **longer payloads only for new `msg_type` values**; receivers validate `len` per type.

**Backward compatibility:** Boards that only implement Phase C drop unknown `msg_type` or wrong `len` (already safe if `msg_type != GALLERY` is ignored — extend Phase C parser to allow `len >= 8` and branch on type).

### Proposed layout (normative for implementers)

**Type 1 — `EYE_SYNC_TYPE_GALLERY` (unchanged)**  
Length **8**. Fields as in Phase C spec.

**Type 2 — `EYE_SYNC_TYPE_ANIM_SEED`**

| Offset | Size | Field |
|--------|------|--------|
| 0 | 4 | magic `EYE0` |
| 4 | 1 | `msg_type = 2` |
| 5 | 1 | `gallery_idx` (must match current style when sent; follower may validate) |
| 6 | 1 | `flags` (bit 0 tap vs scheduled — reuse semantics from Phase C if combined send) |
| 7 | 1 | `reserved` |
| 8 | 4 | `anim_seed` **uint32_t** LE |

**Total length: 12 bytes.**

**Type 3 — `EYE_SYNC_TYPE_ANIM_PULSE`**

| Offset | Size | Field |
|--------|------|--------|
| 0 | 4 | magic |
| 4 | 1 | `msg_type = 3` |
| 5 | 1 | `gallery_idx` (echo for validation) |
| 6 | 4 | `step` **uint32_t** LE |
| 10 | 2 | `reserved` — send **0** |

**Total length: 12 bytes** (normative). Receivers reject other lengths for `msg_type == 3`.

## Module Boundaries

- **`eye_sync.{h,cpp}`** — Sends/receives new types; schedules leader pulse timer; exposes `eye_sync_anim_on_pulse(step)` or similar **C** callback invoked from tick when a pulse is consumed.
- **`eye_functions.cpp`** — Owns anim state; splits **decision** (pulse) from **interpolation** (`frame()`). Refactor moves `random()` calls in `frame()` that affect **gaze/blink scheduling** into pulse-driven functions fed by anim PRNG.
- **`eye_gallery` / renderer** — No direct ESP-NOW; may call into sync on gallery advance (already broadcasts index).

No changes to **`display_async`** or scanline inner loops.

## Config Defines (planned)

| Define | Purpose |
|--------|---------|
| `EYE_SYNC_ANIM_ENABLE` | `1` Phase B on; `0` Phase C only (anim still local-only). |
| `EYE_SYNC_ANIM_PULSE_MS` | Leader pulse period (default 100). |
| `EYE_SYNC_ANIM_LOG` | Optional serial diagnostics. |

When `EYE_SYNC_ANIM_ENABLE` is `0`, pulse and seed types are not sent and inbound anim messages are ignored.

## Testing

| Test | Expected |
|------|----------|
| Two boards, left = leader | Gaze and blinks remain correlated for several minutes; logs show `step` advancing on both. |
| Cover antenna / distance | Brief loss causes at most short lag; after packets resume, follower catches up via `step` gap logic without manual reset. |
| Gallery tap on follower | Phase C index sync + Phase B anim epoch reset; both eyes reset motion together within one pulse period. |
| Gallery tap on leader | Same. |
| `EYE_SYNC_ENABLE 0` | No WiFi; build succeeds. |
| `EYE_SYNC_ANIM_ENABLE 0` | Behaves as Phase C only. |

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Refactor regressions in `frame()` / blink FSM | Golden-eye manual tests; small commits; keep interpolation math byte-for-byte where possible. |
| Pulse flood / air contention | 10 Hz default is low; coexists with Phase C 2 s heartbeat. |
| Leader mis-identified | Document MAC programming; optional future force-leader define. |
| Clock skew between boards | Pulse carry authoritative `step`; wall `micros()` only for interpolation **within** a step. |

## Relation to Phase C Spec

Phase C § “Phase B Forward Compatibility” assumed seed-only on an **8-byte** message. This spec **extends** payloads for types 2–3 while keeping type 1 at **8 bytes**. Phase C `reserved` byte remains usable for gallery sequence if needed; anim seed uses the **12-byte** layout above.

## Acceptance Criteria

1. With Phase B enabled on both boards, **subjective** pairing: casual observer sees **synchronized** look direction and blinking over **≥ 2 minutes** without manual reset.
2. Radio load: **≤ 15 anim packets/s** average on the leader (10 Hz pulse + occasional seed).
3. FPS regression: **≤ 1 FPS** vs Phase C baseline on the same build flags.
4. Single-eye and Phase-C-only builds remain **clean** and documented.

## Out of Scope

- Encryption, multi-pair, BLE/Wi-Fi provisioning UI.
- Changing Phase C gallery wire format for 8-byte `TYPE_GALLERY` messages.

---

*Next step after review: invoke **writing-plans** to produce `docs/superpowers/plans/2026-04-29-eye-sync-phase-b.md` and implement on `feat/eye-sync-phase-b` (or similar) per [`AGENTS.md`](../../../AGENTS.md).*
