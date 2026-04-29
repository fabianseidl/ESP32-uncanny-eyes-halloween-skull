# Eye Sync Phase B — Animation lockstep (implementation plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Phase B animation sync on top of Phase C: shared anim seed plus ~10 Hz leader `TYPE_ANIM_PULSE` so gaze, autoblink, and iris `random()` streams stay correlated across two boards (see [`../specs/2026-04-29-eye-sync-phase-b-design.md`](../specs/2026-04-29-eye-sync-phase-b-design.md)).

**Architecture:** Extend `eye_sync` for 12-byte ESP-NOW payloads (types 2–3), keep 8-byte `TYPE_GALLERY`. Left eye (`g_eye_side == EYE_SIDE_LEFT`) transmits `ANIM_SEED` on gallery epoch and periodic `ANIM_PULSE` with monotonic `step`. Follower drains pulses, advances a dedicated anim PRNG once per step, and runs a **pulse-quantized** gaze/blink FSM so random draws are not tied to physical frame count. `frame()` keeps per-`micros()` easing for motion and blink envelopes; `split()` / `updateEye()` keep using Arduino `random()` after `randomSeed()` from the same epoch seed.

**Tech Stack:** Arduino-ESP32, `arduino-cli compile`, existing `eye_sync` / `eye_gallery` / `eye_functions` / `eye_side` (`g_eye_side`).

---

## File map

| File | Role |
|------|------|
| [`config.h`](../../config.h) | `EYE_SYNC_ANIM_ENABLE`, `EYE_SYNC_ANIM_PULSE_MS`, `EYE_SYNC_ANIM_LOG`; gated on `EYE_SYNC_ENABLE`. |
| [`eye_sync.h`](../../eye_sync.h) | `EYE_SYNC_TYPE_ANIM_SEED`, `EYE_SYNC_TYPE_ANIM_PULSE`, packed 12-byte structs, `EYE_SYNC_WIRE_MAX`;.rx helpers. |
| [`eye_sync.cpp`](../../eye_sync.cpp) | Variable-length RX ring (`MAX(8,12)` payload), parse types 2–3, leader pulse timer, send anim packets, call `eye_anim_*` hooks. |
| [`eye_anim.h`](../../eye_anim.h) **new** | C-callable API: epoch reset, pulse apply, frame hook for interpolation-only, init. |
| [`eye_anim.cpp`](../../eye_anim.cpp) **new** | Anim PRNG (LCG), pulse FSM (dwell + motion + blink scheduling), state extracted from `frame()`. |
| [`eye_functions.cpp`](../../eye_functions.cpp) | Remove gaze/blink `random()` from `frame()`; delegate to `eye_anim`; keep `drawEye`, `split`, `updateEye` integration; include `eye_side.h`. |
| [`eye_gallery.cpp`](../../eye_gallery.cpp) | No change required if epoch is entirely driven from `eye_sync` (seed send paths piggyback on `broadcast_index`). |
| [`README.md`](../../README.md) | Phase B bullets, flags, manual test table. |
| [`docs/superpowers/specs/2026-04-29-eye-sync-phase-b-design.md`](../specs/2026-04-29-eye-sync-phase-b-design.md) | After implementation: set status to **implemented** (separate doc commit). |

**Out of scope for this plan:** `display_async`, generated gallery assets, partition scheme changes.

---

## Branching

Per [`AGENTS.md`](../../AGENTS.md):

```bash
git checkout feat/eye-sync-phase-b-spec   # contains the Phase B spec commit
git checkout -b feat/eye-sync-phase-b
```

Implementation commits live on `feat/eye-sync-phase-b`. Merge to `main` with `--no-ff` when done.

---

## FSM notes (normative for Task 6)

Pulse-driven gaze replaces **microsecond idle dwell** `random(3000000)` with **`idle_pulses_remaining`** drawn from a bounded range each time a move **completes** (e.g. `anim_prng_range(15, 35)` at 100 ms ≈ 1.5–3.5 s, tunable to match legacy feel).

Motion **duration** stays in **microseconds** (`random(72000, 144000)` equivalent via `anim_prng_range`) so easing math in `frame()` stays unchanged.

**Autoblink:** Replace `timeToNextBlink` / `random(4000000)` chain with **pulse countdown** `blink_wait_pulses` decremented each pulse; when zero, same blink start logic as today (`ENBLINK`, durations from PRNG). Envelope inside `frame()` still uses `micros()` against `eye.blink.startTime`.

When `EYE_SYNC_ANIM_ENABLE` is **0**, `eye_anim` falls back to **legacy** behavior: all decisions remain inside `frame()` using `random()` (copy pre-refactor paths into `#else` or a single `if (!anim_sync)` block).

**Degraded mode (follower, no pulse):** If `EYE_SYNC_ANIM_ENABLE 1` and `g_eye_side != EYE_SIDE_LEFT` and **no** valid pulse received for **`EYE_SYNC_ANIM_FALLBACK_MS`** (new define, default `4000`), switch follower to legacy local `random()` until pulses resume (document in README). Leader never uses degraded mode for pulses it generates locally.

---

### Task 1: Config toggles

**Files:**

- Modify: [`config.h`](../../config.h)

- [ ] **Step 1: Add Phase B defines after Phase C block**

Insert only when `EYE_SYNC_ENABLE` is 1 in your mental model; physically add defines near existing `EYE_SYNC_*`:

```c
// --- Eye sync phase B (animation lockstep) --------------------------------
// Requires EYE_SYNC_ENABLE 1. When 0, no anim sync code path is compiled in
// eye_anim / eye_sync extensions.
#ifndef EYE_SYNC_ANIM_ENABLE
#define EYE_SYNC_ANIM_ENABLE  1
#endif
#ifndef EYE_SYNC_ANIM_PULSE_MS
#define EYE_SYNC_ANIM_PULSE_MS  100
#endif
#ifndef EYE_SYNC_ANIM_FALLBACK_MS
#define EYE_SYNC_ANIM_FALLBACK_MS  4000
#endif
#ifndef EYE_SYNC_ANIM_LOG
#define EYE_SYNC_ANIM_LOG  0
#endif
```

Use `#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE` in `.cpp` files that reference anim sync.

- [ ] **Step 2: Commit**

```bash
git add config.h
git commit -m "feat(eye-sync): config toggles for Phase B animation sync"
```

---

### Task 2: Wire structs and `eye_sync.h`

**Files:**

- Modify: [`eye_sync.h`](../../eye_sync.h)

- [ ] **Step 1: Append types and length constant**

After `EYE_SYNC_TYPE_GALLERY` add:

```c
#define EYE_SYNC_TYPE_ANIM_SEED   2u
#define EYE_SYNC_TYPE_ANIM_PULSE  3u

#define EYE_SYNC_WIRE_MAX  12u

struct __attribute__((packed)) EyeSyncMsgAnimSeed {
  uint8_t  magic[4];
  uint8_t  msg_type;   // EYE_SYNC_TYPE_ANIM_SEED
  uint8_t  gallery_idx;
  uint8_t  flags;
  uint8_t  reserved;
  uint32_t anim_seed;  // little-endian on wire
};
static_assert(sizeof(EyeSyncMsgAnimSeed) == 12, "EyeSyncMsgAnimSeed");

struct __attribute__((packed)) EyeSyncMsgAnimPulse {
  uint8_t  magic[4];
  uint8_t  msg_type;   // EYE_SYNC_TYPE_ANIM_PULSE
  uint8_t  gallery_idx;
  uint32_t step;       // LE — wire bytes 6–9 per design spec
  uint16_t reserved;   // send 0 — wire bytes 10–11
};
static_assert(sizeof(EyeSyncMsgAnimPulse) == 12, "EyeSyncMsgAnimPulse");
```

Add forward declarations / includes only as needed; [`eye_anim.h`](../../eye_anim.h) declares `eye_anim_reset_epoch`, `eye_anim_on_pulse`, `eye_anim_is_degraded`.

Expose **no** anim hooks in `eye_sync.h` except any public API you need from `.ino` (prefer none).

- [ ] **Step 2: Commit**

```bash
git add eye_sync.h
git commit -m "feat(eye-sync): Phase B wire structs (anim seed + pulse)"
```

---

### Task 3: `eye_anim` module skeleton

**Files:**

- Create: [`eye_anim.h`](../../eye_anim.h)
- Create: [`eye_anim.cpp`](../../eye_anim.cpp)

- [ ] **Step 1: Create `eye_anim.h`**

```c
#pragma once
#include <stdint.h>
#include "config.h"

void eye_anim_init(void);

#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE
void eye_anim_reset_epoch(uint32_t anim_seed);
void eye_anim_on_pulse(uint32_t step);
bool eye_anim_is_degraded(void);
#else
static inline void eye_anim_reset_epoch(uint32_t anim_seed) { (void)anim_seed; }
static inline void eye_anim_on_pulse(uint32_t step) { (void)step; }
static inline bool eye_anim_is_degraded(void) { return true; }
#endif
```

Task 6 adds `eye_anim_prepare_for_draw(...)` (or equivalent); Task 3 uses only the stubs above so the sketch compiles.

- [ ] **Step 2: Create `eye_anim.cpp` stub**

```cpp
#include "config.h"
#include "eye_anim.h"

void eye_anim_init(void) {}

#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE
void eye_anim_reset_epoch(uint32_t anim_seed) { (void)anim_seed; }
void eye_anim_on_pulse(uint32_t step) { (void)step; }
bool eye_anim_is_degraded(void) { return true; }
#endif
```

- [ ] **Step 3: Verify compile**

Run from repo root (adjust `FQBN` to match [`README.md`](../../README.md)):

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app"
arduino-cli compile --fqbn "$FQBN" .
```

Expected: **success** (once `eye_anim.cpp` is added to sketch — Arduino picks up all `.cpp` in folder).

- [ ] **Step 4: Commit**

```bash
git add eye_anim.h eye_anim.cpp
git commit -m "feat(eye_anim): stub module for Phase B"
```

---

### Task 4: RX variable-length queue and parsing

**Files:**

- Modify: [`eye_sync.cpp`](../../eye_sync.cpp)

- [ ] **Step 1: Replace `EyeSyncMsg` ring with byte buffer**

Use a struct per slot:

```c
typedef struct {
  uint8_t len;
  uint8_t mac[6];
  uint8_t payload[EYE_SYNC_WIRE_MAX];
} EyeSyncRxSlot;

static volatile EyeSyncRxSlot s_rx_queue[EYE_SYNC_RX_QSIZE];
```

`on_recv_cb`: if `len < 8 || len > (int)EYE_SYNC_WIRE_MAX` drop. Copy `len` bytes into `payload`, copy MAC, advance head.

- [ ] **Step 2: Drain loop**

In `eye_sync_tick`, for each slot:

1. Verify magic + `payload[4]` == `msg_type`.
2. **Type 1 (`GALLERY`)** — require `len == 8`; cast to `EyeSyncMsg`; existing gallery apply logic unchanged.
3. **Type 2 (`ANIM_SEED`)** — require `len == 12`; validate `gallery_idx` matches `s_local_index` **or** call `eye_gallery_apply_remote_index` first if remote gallery change ordering requires it (same race-guard as gallery for **index** field); then `eye_anim_reset_epoch(anim_seed)` and `randomSeed(anim_seed)` inside reset or inside `eye_anim_reset_epoch`.
4. **Type 3 (`ANIM_PULSE`)** — require `len == 12`; if `gallery_idx != s_local_index` drop; decode LE `step`; if `EYE_SYNC_ANIM_ENABLE` and not leader (or leader **also** applies locally **before** TX—see Task 5) call `eye_anim_on_pulse(step)`.

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: **success**.

- [ ] **Step 4: Commit**

```bash
git add eye_sync.cpp
git commit -m "feat(eye_sync): variable-length RX for Phase B messages"
```

---

### Task 5: TX paths (seed + pulse) and leader timing

**Files:**

- Modify: [`eye_sync.cpp`](../../eye_sync.cpp)
- Modify: [`eye_sync.h`](../../eye_sync.h) if you add internal helpers

- [ ] **Step 1: Add file-static leader state**

```c
#include "eye_side.h"
#include "eye_anim.h"

static uint32_t s_anim_step = 0;
static uint32_t s_last_pulse_ms = 0;
```

- [ ] **Step 2: Helper `eye_sync_send_anim_seed(uint32_t seed)`**

Build `EyeSyncMsgAnimSeed`, `esp_now_send` broadcast. Call from **`eye_sync_broadcast_index`** when `EYE_SYNC_ANIM_ENABLE` **and** `g_eye_side == EYE_SIDE_LEFT`, **after** `send_msg` for gallery tap — also reset local epoch: `randomSeed(seed); eye_anim_reset_epoch(seed); s_anim_step = 0;` (leader); send seed with `gallery_idx = idx`, `flags` tap if applicable.

Generate `seed` with e.g. `esp_random()` ^ (mac[0] << 8) (include `<esp_random.h>`).

- [ ] **Step 3: Leader pulse tick**

At end of `eye_sync_tick`, if anim enabled and leader and inited:

```c
if ((uint32_t)(now - s_last_pulse_ms) >= (uint32_t)EYE_SYNC_ANIM_PULSE_MS) {
  s_last_pulse_ms = now;
  s_anim_step++;
  eye_anim_on_pulse(s_anim_step);
  // build EyeSyncMsgAnimPulse, send broadcast
}
```

Initialize `s_last_pulse_ms = millis()` in `eye_sync_init`.

- [ ] **Step 4: Follower-only pulse handling**

Do **not** double-apply on leader from RX of own broadcast if ESP-NOW echoes self—if self-RX occurs, drop packets where `src_mac` equals STA MAC.

- [ ] **Step 5: Compile + commit**

```bash
arduino-cli compile --fqbn "$FQBN" .
git add eye_sync.cpp eye_anim.cpp eye_anim.h eye_sync.h config.h
git commit -m "feat(eye-sync): transmit anim seed and leader pulses"
```

---

### Task 6: Implement `eye_anim` FSM and wire `eye_functions`

**Files:**

- Modify: [`eye_anim.cpp`](../../eye_anim.cpp)
- Modify: [`eye_anim.h`](../../eye_anim.h)
- Modify: [`eye_functions.cpp`](../../eye_functions.cpp)

- [ ] **Step 1: Move static gaze/blink driver state**

From `frame()` in [`eye_functions.cpp`](../../eye_functions.cpp) (~lines 212–259), relocate **`eyeInMotion`, `eyeOldX`/`Y`, `eyeNewX`/`Y`, `eyeMoveStartTime`, `eyeMoveDuration`** and autoblink scheduling fields into `eye_anim.cpp` as file-static, or keep blink globals in `eye_functions` if `AUTOBLINK` timing moves to `eye_anim` only—**consolidate** in `eye_anim` for sync build.

- [ ] **Step 2: PRNG helpers**

```c
static uint32_t s_anim_lcg;
static uint32_t anim_prng_u32(void) {
  s_anim_lcg = s_anim_lcg * 1664525u + 1013904223u;
  return s_anim_lcg;
}
static int32_t anim_prng_range(int32_t lo, int32_t hi) {
  if (hi <= lo) return lo;
  uint32_t r = anim_prng_u32();
  return lo + (int32_t)(r % (uint32_t)(hi - lo));
}
```

`eye_anim_reset_epoch`: `s_anim_lcg = anim_seed ^ 0xA53C965Au; randomSeed(anim_seed);` (iris path); reset motion to centered 512/512, `eyeInMotion = false`, idle pulses = small constant, **do not** leave `timeOfLastBlink` inconsistent—reset autoblink baseline per spec (`micros()` snapshot).

`eye_anim_on_pulse` when `EYE_SYNC_ANIM_ENABLE`:

1. `anim_prng_u32();` once (or use the u32 as part of FSM—exactly **one** LCG step per pulse).
2. Run dwell counter / motion completion / new-target selection / blink pulse counter per **FSM notes** above.
3. Track `last_step` and **fast-forward** LCG if `step > last_step + 1`:

```c
while (last_step + 1u < step) {
  last_step++;
  anim_prng_u32();
  // optionally run pulse FSM without side effects beyond RNG—simplest: only fast-forward LCG (step - expected) times
}
last_step = step - 1;
// then process `step` once more with full FSM? **Simpler:** set `last_step = step` after consuming `(step - last_step)` LCG advances and `(step - last_step)` FSM ticks in a loop.
```

Document the chosen rule in code comments to match spec “fast-forward”.

- [ ] **Step 3: `eye_anim_frame_apply`**

Export a function called from `frame()` **before** lid threshold math:

```c
eye_anim_prepare_for_draw(uint32_t t, int16_t* eyeX, int16_t* eyeY, uint16_t* iScale_for_blink_path);
```

Inside: compute eased `eyeX`/`eyeY` from existing ease table using `micros` deltas (copy from current `frame()`), map to sclera coords as today, run blink **state machine transitions** that depend only on `micros()` vs `eye.blink.*` (unchanged from current).

When `EYE_SYNC_ANIM_ENABLE` is **0** or `eye_anim_is_degraded()` returns true, compile **legacy** `frame()` branch inline (copy pre-change logic) to avoid behavior regression for single-board users.

- [ ] **Step 4: Remove `random()` from gaze/autoblink in sync path**

In `frame()`, guard:

```cpp
#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE
  if (!eye_anim_is_degraded()) {
    eye_anim_prepare_for_draw(t, &eyeX, &eyeY, &iScale);
    // skip legacy random blocks
  } else
#endif
  { /* legacy random gaze + blink */ }
```

- [ ] **Step 5: Compile both flag combinations**

```bash
arduino-cli compile --fqbn "$FQBN" .
# Second build: set EYE_SYNC_ANIM_ENABLE to 0 in config.h, compile, verify success, then restore 1.
```

Expected: **two successes** (Phase C–only and full Phase B).

- [ ] **Step 6: Commit**

```bash
git add eye_anim.cpp eye_anim.h eye_functions.cpp
git commit -m "feat(eye_anim): pulse FSM and frame integration"
```

---

### Task 7: README + spec status

**Files:**

- Modify: [`README.md`](../../README.md)
- Modify: [`docs/superpowers/specs/2026-04-29-eye-sync-phase-b-design.md`](../specs/2026-04-29-eye-sync-phase-b-design.md)

- [ ] **Step 1: README — document flags, leader rule, manual tests**

Under Pair sync / Phase C section, add **Phase B** subsection: `EYE_SYNC_ANIM_ENABLE`, `EYE_SYNC_ANIM_PULSE_MS`, leader = left MAC, fallback timer, expected serial strings if `EYE_SYNC_ANIM_LOG`.

- [ ] **Step 2: Spec status line**

Change **Status** to `implemented (branch feat/eye-sync-phase-b, YYYY-MM-DD)` after verification on hardware.

- [ ] **Step 3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-04-29-eye-sync-phase-b-design.md
git commit -m "docs: Phase B usage and spec status"
```

---

## Spec coverage (self-review)

| Spec section | Tasks |
|--------------|-------|
| Leader = left `g_eye_side` | Task 5 |
| Seed + pulse transports | Tasks 2, 4, 5 |
| 12-byte layouts | Task 2 |
| Iris `random()` via `randomSeed` on epoch | Task 5–6 `eye_anim_reset_epoch` |
| Gaze/blink off global `random()` in sync mode | Task 6 |
| `EYE_SYNC_ENABLE 0` / `EYE_SYNC_ANIM_ENABLE 0` | Tasks 1, 3 stubs, 6 legacy branch |
| Hot path / no display_async changes | File map |
| Fallback when follower starved | Task 6 `eye_anim_is_degraded` |
| FPS ≤ +1 regression | Manual: compare `FPS=` serial before/after |

**Placeholder scan:** No TBD sections in task code paths; tunables called out with defaults.

**Type consistency:** `EyeSyncMsgAnimPulse` step bytes 6–9 as LE must match encode/decode in `eye_sync.cpp`; use `memcpy` from `uint32_t` with `uint8_t*` for endian safety on ESP32 (little-endian).

---

## Plan complete and saved to `docs/superpowers/plans/2026-04-29-eye-sync-phase-b.md`. Two execution options:

**1. Subagent-Driven (recommended)** — Dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
