# Eye Sync — Phase C: Gallery Index Sync over ESP-NOW

**Status:** implemented (branch `feat/eye-sync-phase-c`)
**Date:** 2026-04-28
**Scope:** Synchronize the runtime eye gallery index between two `ESP32-uncanny-eyes-halloween-skull` boards (left + right eye) so a tap on either board cycles both to the same style. Animation sync (eye motion, blink, iris) is explicitly Phase B and out of scope here.

## Corrigenda (discovered during implementation)

Two adjustments to the original design were required to make this work in practice. Read these before relying on the spec text below.

1. **`eye_sync_tick()` must also be polled from inside `frame()`, not only from `loop()`.** `updateEye()` calls `split()` recursively for ~10 s per call, blocking `loop()` for that entire window. RX packets queue up but stay undrained, and heartbeats fire late. The fix mirrors the existing touch-poll pattern: `eye_functions.cpp` `frame()` now calls `eye_sync_tick()` right after `eye_gallery_poll_touch_during_render()`. RX latency drops from up to ~10 s to ~1 frame (~45 ms at ~22 FPS); race-guard window math (which depends on prompt drain) becomes meaningful again. The `loop()` call site is retained as the canonical entry point but contributes negligibly under the current animation timing.
2. **Build requires `PartitionScheme=huge_app`, not `default`.** Linking WiFi + ESP-NOW grows the binary to ~1.83 MB, which overflows the 1.31 MB `default` app partition. `huge_app` provides a ~3 MB app partition and drops the (unused) OTA slot. The single-eye fallback build (`EYE_SYNC_ENABLE 0`, ~1.27 MB) still fits in `default` for users who do not need pair sync.

## Background and Motivation

The current firmware drives one eye per board. The README ([README.md:147](../../../README.md#L147)) flags pair sync as a known gap and suggests ESP-NOW with a shared RNG seed as a future direction. For a Halloween skull with two eyes, divergent gallery state is immediately visible (cat eye + owl eye looks broken) and visually worse than divergent animation timing. We therefore split the work:

- **Phase C (this spec):** Both boards always show the same gallery index. Animation runs independently per board.
- **Phase B (later):** Both eyes animate "alive together" — similar look directions, blinking around the same time. Built on the same transport this spec establishes.

Starting with C gives us a real, shippable feature, exercises the entire ESP-NOW pipeline end-to-end, and produces no throwaway code: Phase B extends the same transport with additional message types.

## Goals and Non-Goals

**Goals (Phase C):**

- A tap on either board cycles **both** boards to the next gallery style.
- Boards survive an asymmetric boot (one powered up later than the other) and resynchronize automatically.
- Boards survive an occasional dropped ESP-NOW packet without manual intervention.
- The same firmware image flashes onto both boards (only `EYE_SIDE` differs in `config.h`).
- A compile-time switch reverts to the current single-eye behavior with no WiFi/ESP-NOW code linked in.
- No regression in render path: FPS impact ≤ 1 FPS.

**Non-Goals:**

- Animation sync (eye direction, blink, iris). Deferred to Phase B.
- Encryption / authentication of the link. No realistic threat for a residential Halloween prop.
- Web UI / WiFi-AP-based configuration. Overkill.
- Support for more than two boards in a peer group. Two-eye assumption baked in (broadcast topology naturally extends, but is not validated).
- Deterministic conflict resolution for two simultaneously-tapped boards within the same millisecond (acceptably rare; one tap "loses" for up to 2 s until the next heartbeat reconciles).

## High-Level Architecture

Both boards run WiFi in `WIFI_STA` mode without associating with any AP, locked to a fixed channel. ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) is the transport. Every message carries a 4-byte magic prefix (`'EYE0'`) so foreign ESP-NOW traffic is filtered.

Three event sources can change a board's gallery state:

1. **Local tap (CST9217 touch or serial `n`/CR/LF).** Existing `eye_gallery` advance logic runs first; afterwards a new hook calls `eye_sync_broadcast_index(new_idx)`.
2. **Remote update via ESP-NOW.** The receive callback validates the message, applies a small race guard (described below), and on success calls `eye_gallery_apply_remote_index(idx)` — a new function that updates the gallery pointer **without** triggering another broadcast (no feedback loop).
3. **Periodic heartbeat.** Every 2 s, each board broadcasts its current index. A board that just booted, missed a tap message, or got reset catches up on the next heartbeat from its peer.

The renderer (`eye_functions.cpp`) and touch driver (`eye_gallery.cpp` touch path) know nothing about sync. All ESP-NOW logic lives in a new `eye_sync.{h,cpp}` module. The only seam in `eye_gallery.cpp` is one new exported function (`apply_remote_index`) and one new call site (after a local advance).

## Wire Format

Fixed 8-byte packed struct, sent as the entire ESP-NOW payload:

```c
// eye_sync.h
#define EYE_SYNC_MAGIC         { 'E', 'Y', 'E', '0' }
#define EYE_SYNC_TYPE_GALLERY  1
// Reserved for Phase B: TYPE_ANIM_SEED = 2, TYPE_HEARTBEAT_ANIM = 3, ...

#define EYE_SYNC_FLAG_TAP      0x01  // bit 0: tap-triggered (else heartbeat)

struct __attribute__((packed)) EyeSyncMsg {
  uint8_t magic[4];   // 'E','Y','E','0' — filter foreign traffic
  uint8_t msg_type;   // discriminator; Phase C only emits / accepts TYPE_GALLERY
  uint8_t index;      // gallery index 0..N-1
  uint8_t flags;      // bit 0 = tap-triggered; bits 1..7 reserved (= 0)
  uint8_t reserved;   // pad to 8 bytes; reserved for Phase B sequence number
};
static_assert(sizeof(EyeSyncMsg) == 8, "wire format");
```

Field rationale:

- **`magic`** — cheap filter so we ignore ESP-NOW from unrelated projects on the same channel.
- **`msg_type`** — keeps Phase B additions strictly forward-compatible. A Phase C board receiving a `TYPE_ANIM_SEED` packet ignores it cleanly.
- **`index`** — 1 byte covers the current 6-style gallery and any plausible expansion.
- **`flags` bit 0** — lets the receiver and serial logs distinguish a tap-driven update from a heartbeat. Phase C uses this only for logging clarity; Phase B may use it for prioritization.
- **`reserved`** — explicit pad for a future sequence number without breaking the wire format.

ESP-NOW broadcast frame overhead is roughly 25 bytes; the total over-the-air burst is ~33 bytes per message. With one heartbeat every 2 s per board, link load is ~33 B/s per board — negligible.

## Module API

A small public surface keeps the renderer and gallery free of ESP-NOW knowledge:

```c
// eye_sync.h
void eye_sync_init(void);                    // setup(): WiFi STA + ESP-NOW + broadcast peer
void eye_sync_tick(void);                    // loop(): heartbeat scheduling + RX drain
void eye_sync_broadcast_index(uint8_t idx);  // called from eye_gallery on local advance
```

Internal state (file-static in `eye_sync.cpp`):

```c
static uint8_t  s_local_index;            // mirror of the gallery's current index
static uint32_t s_last_local_change_ms;   // millis() of most recent local tap
static uint32_t s_last_heartbeat_ms;      // millis() of most recent self-broadcast
```

A small ring buffer (e.g. 4 slots) holds messages received in the ESP-NOW callback context; `eye_sync_tick()` drains it on the main loop to avoid touching gallery state from an interrupt-style callback.

## State Machine

**Receive path (`on_recv_cb` → enqueue; drained in `eye_sync_tick`):**

```
1. payload length != 8     → drop
2. magic != 'EYE0'         → drop (foreign traffic)
3. msg_type != GALLERY     → drop (reserved for Phase B)
4. msg.index == s_local_index → drop (already in sync)
5. (millis() - s_last_local_change_ms) < EYE_SYNC_RACE_GUARD_MS
                           → drop (we just tapped; our broadcast wins the race)
6. eye_gallery_apply_remote_index(msg.index)
   (updates g_eye + the gallery's internal index, does NOT call broadcast)
   s_local_index = msg.index
```

**Tick (called once per `loop()`):**

```
- drain RX ring buffer (apply receive logic above to each entry)
- if (millis() - s_last_heartbeat_ms) >= EYE_SYNC_HEARTBEAT_MS:
    send EyeSyncMsg{ magic, TYPE_GALLERY, s_local_index, flags=0, 0 }
    s_last_heartbeat_ms = millis()
```

**Local tap (`eye_sync_broadcast_index(new_idx)`):**

```
s_local_index           = new_idx
s_last_local_change_ms  = millis()
s_last_heartbeat_ms     = millis()    // suppresses immediate redundant heartbeat
send EyeSyncMsg{ magic, TYPE_GALLERY, new_idx, flags=EYE_SYNC_FLAG_TAP, 0 }
```

**Race-guard rationale:** Without the guard, this race exists: board A taps, broadcasts; before the broadcast reaches board B, board B's heartbeat (carrying the *old* index) lands at A and would overwrite A's freshly-tapped state. The 500 ms guard suppresses *receiving* updates within a window after a local tap. Heartbeats from A continue to flow, so B will reconcile within 2 s.

## Integration Points

**`config.h`** gains four new defines:

```c
#define EYE_SYNC_ENABLE         1     // 0 = single-eye build (no WiFi linked)
#define EYE_SYNC_CHANNEL        1     // WiFi channel; both boards must agree
#define EYE_SYNC_HEARTBEAT_MS   2000  // heartbeat interval
#define EYE_SYNC_RACE_GUARD_MS  500   // ignore incoming for this window after local tap
#define EYE_SYNC_LOG            1     // 0 = silent; 1 = serial diagnostics
```

`EYE_SYNC_ENABLE 0` must produce a build that compiles without WiFi/ESP-NOW headers and links no extra code — the entire `eye_sync.cpp` body sits behind `#if EYE_SYNC_ENABLE` and the call sites in `.ino` / `eye_gallery.cpp` similarly compile out. This preserves the current single-eye behavior as a fallback and bounds the flash-pressure risk.

**`ESP32-uncanny-eyes-halloween-skull.ino`:**
- After `eye_gallery_init()` in `setup()`: `eye_sync_init()`.
- In `loop()`: `eye_sync_tick()` once per iteration. Cheap (timestamp comparison plus an empty queue most ticks).

**`eye_gallery.cpp`:**
- Existing `advance_to_next()` (or whatever the touch / serial path calls): after switching, call `eye_sync_broadcast_index(s_index)`.
- New exported function `eye_gallery_apply_remote_index(uint8_t idx)`: same body as the local advance *minus* the broadcast call. This is the only place that updates state without broadcasting.

**No changes to:** `eye_functions.cpp`, `display_async.cpp`, `display.ino`, the renderer pipeline. Phase C is invisible to the hot path.

## ESP-NOW Initialization

```c
WiFi.mode(WIFI_STA);
esp_wifi_set_channel(EYE_SYNC_CHANNEL, WIFI_SECOND_CHAN_NONE);
esp_now_init();
esp_now_register_recv_cb(on_recv_cb);

esp_now_peer_info_t peer = {};
peer.channel = EYE_SYNC_CHANNEL;
peer.encrypt = false;
memcpy(peer.peer_addr, broadcast_addr, 6);  // FF:FF:FF:FF:FF:FF
esp_now_add_peer(&peer);
```

No AP association. No DHCP. The board never leaves the configured channel because nothing else manipulates the WiFi state.

## Build Impact

- **Flash:** WiFi + ESP-NOW pulls roughly 80–120 KB. The current default-partition build sits at ~94 % full ([README.md:107](../../../README.md#L107)). This is the leading risk; see "Risks" below.
- **RAM:** ESP-NOW itself is small (<10 KB). WiFi init reserves ~30–40 KB heap. With 8 MB PSRAM on board, unproblematic.
- **Active current draw:** WiFi STA active (even unassociated) adds ~80–120 mA. Trivial under USB-C; relevant only if anyone later runs the skull off battery.
- **Boot delay:** WiFi init costs ~200–500 ms before the renderer starts. Acceptable.
- **Per-frame cost:** `eye_sync_tick()` is a timestamp check plus (rarely) a small queue drain. Expected FPS impact ≤ 1 FPS.
- **Dependencies:** Only Espressif headers (`WiFi.h`, `esp_now.h`, `esp_wifi.h`) bundled with the ESP32 Arduino core ≥ 3.3.5 (already required). **No new Arduino libraries.**

## Verification Plan

Manual checks (the test harness for this project is the physical pair of boards):

| Test | Expected |
|------|----------|
| Both boards powered simultaneously, no taps | Both on default style (`nauga`); serial shows heartbeat tx every 2 s on both |
| Tap board A | A advances immediately; B follows within < 100 ms (single broadcast hop) |
| Tap board B | Symmetric to above |
| Boot A, tap A three times, then boot B | B starts on `nauga`, adopts A's index within ≤ 3 s on first heartbeat received |
| Reset B while A is running | After B's WiFi init + first received heartbeat (≤ 3 s), B is back in sync |
| Boards ~1 m apart, hand passing between them | No interruption (ESP-NOW penetrates household obstacles fine) |
| Build with `EYE_SYNC_ENABLE 0` | Compiles cleanly; binary size noticeably smaller; runtime identical to current single-eye behavior |

Serial diagnostics (`EYE_SYNC_LOG 1`):

- `eye_sync: init ok ch=1 mac=AA:BB:CC:DD:EE:FF`
- `eye_sync: tx idx=2 flag=tap rc=0` / `flag=hb`
- `eye_sync: rx idx=2 from=11:22:33:44:55:66 flag=tap` (followed by `eye_gallery: <- owl` from the gallery on apply, or `eye_sync:   ignore (race-guard)` when suppressed; in-sync messages drop silently)

## Acceptance Criteria

1. Local tap propagates to the peer in < 100 ms (best case) and < 2 s (worst case under single-packet loss).
2. Asymmetric boot or single-board reset re-syncs within ≤ 3 s.
3. `EYE_SYNC_ENABLE 0` build is clean and behaves identically to the current Phase 2 single-eye firmware.
4. Renderer FPS regression ≤ 1 FPS measured against the current ~17 FPS baseline.
5. Same firmware image flashes to both boards with only `EYE_SIDE` differing.

## Risks and Mitigations

- **Flash overflow on the default partition.** Most likely failure mode. Mitigation: take a compile probe immediately after the bare-bones sync module compiles. If overflow: either bump to a larger partition scheme (e.g. `huge_app`) or drop one gallery style. Decision deferred to first compile, not a design blocker.
- **Channel mismatch between boards.** Catastrophic — they'd never see each other. Mitigation: single source of truth (`EYE_SYNC_CHANNEL`) in `config.h`; both boards built from the same checkout.
- **Future WiFi feature accidentally hops the channel.** If anyone later adds AP-association code, the channel may drift. Mitigation: documented invariant — Phase C *requires* a fixed, manually-set channel. Any feature that connects to an AP must explicitly re-pin or re-add the broadcast peer on the new channel.
- **Simultaneous taps within the same millisecond on both boards.** No deterministic winner; one tap is overwritten on the next heartbeat (≤ 2 s). Acceptable for a Halloween prop; revisitable in Phase B by populating the `reserved` byte with a sequence number.
- **ESP-NOW callback executes in a high-priority context.** Doing gallery updates directly from the callback is unsafe (touches PROGMEM pointer state shared with the renderer). Mitigation: callback only enqueues; `eye_sync_tick()` drains on the main loop.

## Phase B Forward Compatibility (informational)

Phase B will extend the same transport with at least one new message type, e.g. `TYPE_ANIM_SEED` carrying a shared RNG seed plus a synchronization timestamp. The renderer will reseed `random()` from this and run the existing animation loop deterministically; both boards then move and blink "together" without per-frame messaging. Because Phase C reserves `msg_type` and the unused `reserved` byte, Phase B requires no wire-format break.

## Out of Scope (Explicit)

- Animation sync (Phase B).
- Encryption / authentication.
- Multi-pair coordination (more than two boards).
- Configurability via web UI or BLE provisioning.
- Deterministic resolution of sub-millisecond simultaneous taps.
