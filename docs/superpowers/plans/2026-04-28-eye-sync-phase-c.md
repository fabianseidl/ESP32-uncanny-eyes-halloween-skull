# Eye Sync (phase C — gallery index sync over ESP-NOW) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two boards running this firmware show the same gallery style at all times. A tap on either board cycles both boards to the next style. Asymmetric boot or single-packet loss self-heals within ~2 s. Animation sync is **out of scope** (phase B).

**Architecture:** New module `eye_sync.{h,cpp}` owns all ESP-NOW + WiFi code. Both boards run `WIFI_STA` (no AP association) on a fixed channel and broadcast 8-byte messages with a 4-byte magic prefix. `eye_gallery.cpp` gets one new exported function (`eye_gallery_apply_remote_index`) and one new call site (after a local advance) — that's the entire seam in existing code. The renderer (`eye_functions.cpp`, `display_*`) is untouched. Compile-time switch `EYE_SYNC_ENABLE` reduces the build to the current single-eye behavior with no WiFi code linked.

**Tech Stack:** Arduino + `arduino-cli`, ESP32-S3, ESP-NOW from the ESP32 Arduino core (`WiFi.h`, `esp_now.h`, `esp_wifi.h`). **No new Arduino libraries.** ESP32 Arduino core ≥ 3.3.5 (already required by this repo).

**Verification:** No host unit tests. After each code task: `arduino-cli compile` with the FQBN from [`README.md`](../../../README.md). Hardware checkpoints require **two boards** powered simultaneously (Task 4 onward); single-board probes are still useful before that.

**Spec:** [`docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md`](../specs/2026-04-28-eye-sync-phase-c-design.md).

**Branch (per [`AGENTS.md`](../../../AGENTS.md)):** `git checkout -b feat/eye-sync-phase-c` before the first commit.

---

## File structure

| File | Responsibility |
|------|----------------|
| `eye_sync.h` (new) | Public API: `eye_sync_init()`, `eye_sync_tick()`, `eye_sync_broadcast_index(idx)`. Wire-format struct `EyeSyncMsg`. Magic / type / flag constants. |
| `eye_sync.cpp` (new) | All ESP-NOW + WiFi code, fully wrapped in `#if EYE_SYNC_ENABLE`. State (`s_local_index`, timestamps), ring buffer for RX, RX callback, heartbeat scheduling. |
| `config.h` (modify) | Add 5 new defines: `EYE_SYNC_ENABLE`, `EYE_SYNC_CHANNEL`, `EYE_SYNC_HEARTBEAT_MS`, `EYE_SYNC_RACE_GUARD_MS`, `EYE_SYNC_LOG`. |
| `eye_gallery.h` (modify) | Add prototype `void eye_gallery_apply_remote_index(uint8_t idx)`. |
| `eye_gallery.cpp` (modify) | Add definition of `eye_gallery_apply_remote_index`. Add one `eye_sync_broadcast_index(...)` call inside `eye_gallery_next()`, guarded by `#if EYE_SYNC_ENABLE`. |
| `ESP32-uncanny-eyes-halloween-skull.ino` (modify) | Call `eye_sync_init()` at end of `setup()` and `eye_sync_tick()` in `loop()`, both guarded by `#if EYE_SYNC_ENABLE`. |
| `README.md` (modify, last task) | One-paragraph "Pair sync" section + serial-line example. |
| `docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md` (modify, last task) | Status: `design` → `implemented`. |

**Out of scope for this plan:** animation sync (phase B), encryption, web/BLE provisioning, three-or-more-board topologies.

---

### Task 1: Branch + config flags

**Files:**
- Modify: `config.h`

- [ ] **Step 1.1: Create branch**

```bash
cd /Users/fabi/dev/ESP32-uncanny-eyes-halloween-skull
git checkout -b feat/eye-sync-phase-c
```

- [ ] **Step 1.2: Add the five sync config defines to `config.h`**

Open [`config.h`](../../../config.h). Find the `IRIS_MIN` / `IRIS_MAX` block at the bottom of the file and append **after** the closing `#endif`:

```c
// --- Eye sync (phase C) ----------------------------------------------------
// Set EYE_SYNC_ENABLE to 0 for the single-eye fallback build (no WiFi code).
// Both boards must share the same channel value.
#define EYE_SYNC_ENABLE         1
#define EYE_SYNC_CHANNEL        1     // WiFi channel both boards use
#define EYE_SYNC_HEARTBEAT_MS   2000  // heartbeat interval per board
#define EYE_SYNC_RACE_GUARD_MS  500   // ignore inbound for this window after local tap
#define EYE_SYNC_LOG            1     // 0 = silent; 1 = serial diagnostics
```

- [ ] **Step 1.3: Compile probe (sketch must still build identically — defines unused)**

```bash
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PSRAM=opi,PartitionScheme=default"
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile. Binary size should be effectively unchanged (a few unused `#define`s have no code impact).

- [ ] **Step 1.4: Commit**

```bash
git add config.h
git commit -m "feat(sync): add EYE_SYNC_* config flags (phase C)"
```

---

### Task 2: `eye_sync.h` — wire format and public API

**Files:**
- Create: `eye_sync.h`

- [ ] **Step 2.1: Create `eye_sync.h` with the verbatim content below**

```c
#pragma once

#include <stdint.h>

// --- Wire format -----------------------------------------------------------
// 8-byte packed message broadcast over ESP-NOW. See
// docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md.
//
// Phase C only emits and accepts msg_type = EYE_SYNC_TYPE_GALLERY. Other
// values are reserved for phase B (animation sync) and must be ignored
// cleanly to keep the wire forward-compatible.

#define EYE_SYNC_MAGIC0  'E'
#define EYE_SYNC_MAGIC1  'Y'
#define EYE_SYNC_MAGIC2  'E'
#define EYE_SYNC_MAGIC3  '0'

#define EYE_SYNC_TYPE_GALLERY  1u

#define EYE_SYNC_FLAG_TAP      0x01u  // bit 0: tap-triggered (else heartbeat)

struct __attribute__((packed)) EyeSyncMsg {
  uint8_t magic[4];   // 'E','Y','E','0'
  uint8_t msg_type;   // EYE_SYNC_TYPE_GALLERY in phase C
  uint8_t index;      // gallery index 0..N-1
  uint8_t flags;      // bit 0 = tap-triggered
  uint8_t reserved;   // pad to 8 bytes; reserved for phase B sequence number
};
static_assert(sizeof(EyeSyncMsg) == 8, "EyeSyncMsg wire format must be 8 bytes");

// --- Public API ------------------------------------------------------------
// All functions are no-ops when EYE_SYNC_ENABLE == 0.

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize WiFi STA + ESP-NOW + broadcast peer. Call once from setup()
 *  AFTER eye_gallery_init() so s_local_index can mirror the start index. */
void eye_sync_init(void);

/** Drain RX queue and (re)send heartbeat if interval elapsed.
 *  Call once per loop() iteration. Cheap: timestamp compare + (rare) queue drain. */
void eye_sync_tick(void);

/** Broadcast the current gallery index immediately. Call from
 *  eye_gallery_next() after a LOCAL advance (touch or serial). Sets the
 *  EYE_SYNC_FLAG_TAP flag and resets the race-guard timer. */
void eye_sync_broadcast_index(uint8_t idx);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2.2: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile. The header is included by nothing yet, so its only effect is being part of the sketch directory.

- [ ] **Step 2.3: Commit**

```bash
git add eye_sync.h
git commit -m "feat(sync): public API and 8-byte wire format"
```

---

### Task 3: `eye_sync.cpp` skeleton (no-op stubs) + setup/loop wiring

**Goal:** Hardware-flashable skeleton. `eye_sync_init()` logs `eye_sync: init ok (stub)` and the tick + broadcast functions are empty. This isolates the .ino wiring from the actual ESP-NOW work — if Task 4 fails, we already know the seams are correct.

**Files:**
- Create: `eye_sync.cpp`
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino`

- [ ] **Step 3.1: Create `eye_sync.cpp` with the verbatim content below**

```c
#include <Arduino.h>

#include "config.h"
#include "eye_sync.h"

#if EYE_SYNC_ENABLE

#ifndef EYE_SYNC_LOG
#define EYE_SYNC_LOG 0
#endif

void eye_sync_init(void) {
#if EYE_SYNC_LOG
  Serial.println("eye_sync: init ok (stub)");
#endif
}

void eye_sync_tick(void) {
  // Filled in Task 5 (TX) and Task 6 (RX).
}

void eye_sync_broadcast_index(uint8_t idx) {
  (void)idx;
  // Filled in Task 5.
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.

void eye_sync_init(void)                    {}
void eye_sync_tick(void)                    {}
void eye_sync_broadcast_index(uint8_t idx)  { (void)idx; }

#endif
```

- [ ] **Step 3.2: Wire `eye_sync_init()` into `setup()` and `eye_sync_tick()` into `loop()`**

Edit [`ESP32-uncanny-eyes-halloween-skull.ino`](../../../ESP32-uncanny-eyes-halloween-skull.ino):

Add the include alongside the existing ones near the top:

```c
#include "eye_sync.h"
```

In `setup()`, after the existing `eye_gallery_touch_begin();` line and **before** `startTime = millis();`, add:

```c
  eye_sync_init();
```

In `loop()`, replace:

```c
void loop() {
  eye_gallery_poll();
  updateEye();
}
```

with:

```c
void loop() {
  eye_gallery_poll();
  eye_sync_tick();
  updateEye();
}
```

- [ ] **Step 3.3: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile.

- [ ] **Step 3.4: Hardware probe (one board is enough)**

Flash and read serial:

```bash
PORT=$(arduino-cli board list | awk '/ESP32 Family Device/{print $1; exit}')
arduino-cli upload --fqbn "$FQBN" -p "$PORT" .
arduino-cli monitor -p "$PORT" -c baudrate=115200
```

Expected serial includes (somewhere after `eye_gallery: touch ok ...`):
```
eye_sync: init ok (stub)
```

If you don't see this line, the `setup()` call wasn't wired. Stop and fix before continuing.

- [ ] **Step 3.5: Commit**

```bash
git add eye_sync.cpp ESP32-uncanny-eyes-halloween-skull.ino
git commit -m "feat(sync): skeleton stubs wired into setup/loop"
```

---

### Task 4: Real `eye_sync_init()` — WiFi STA + ESP-NOW + broadcast peer

**Goal:** After this task, both boards initialize WiFi on the configured channel and add a broadcast peer. No messages are sent or received yet; we just verify the radio and ESP-NOW stack come up cleanly.

**Files:**
- Modify: `eye_sync.cpp`

- [ ] **Step 4.1: Replace the `eye_sync.cpp` body inside `#if EYE_SYNC_ENABLE` with the real init**

Open `eye_sync.cpp`. Replace the entire `#if EYE_SYNC_ENABLE` block (the stub `eye_sync_init` + empty `eye_sync_tick` + empty `eye_sync_broadcast_index`) with this. Keep the `#else` no-op block at the bottom **unchanged**.

```c
#if EYE_SYNC_ENABLE

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "eye_gallery.h"

#ifndef EYE_SYNC_LOG
#define EYE_SYNC_LOG 0
#endif

static const uint8_t s_broadcast_addr[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     s_inited                 = false;
static uint8_t  s_local_index            = 0;
static uint32_t s_last_local_change_ms   = 0;
static uint32_t s_last_heartbeat_ms      = 0;

void eye_sync_init(void) {
  WiFi.mode(WIFI_STA);
  // Channel must be set before esp_now_init() so the radio is parked
  // where both boards expect it. We never associate with an AP, so the
  // channel will not drift.
  esp_wifi_set_channel(EYE_SYNC_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
#if EYE_SYNC_LOG
    Serial.println("eye_sync: esp_now_init FAILED");
#endif
    return;
  }

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, s_broadcast_addr, 6);
  peer.channel = EYE_SYNC_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
#if EYE_SYNC_LOG
    Serial.println("eye_sync: add_peer FAILED");
#endif
    return;
  }

  s_inited            = true;
  s_local_index       = 0;  // gallery starts at 0; tap broadcasts will update.
  s_last_heartbeat_ms = millis();

#if EYE_SYNC_LOG
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("eye_sync: init ok ch=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                EYE_SYNC_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

void eye_sync_tick(void) {
  if (!s_inited) {
    return;
  }
  // RX drain + heartbeat — filled in Tasks 5 and 6.
}

void eye_sync_broadcast_index(uint8_t idx) {
  (void)idx;
  // Filled in Task 5.
}

#else  // EYE_SYNC_ENABLE == 0 — fallback no-ops, no WiFi code linked.
```

The `#else` block (single-line stubs) and the closing `#endif` must remain at the bottom of the file unchanged.

- [ ] **Step 4.2: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile. **Binary size will jump significantly** (WiFi + ESP-NOW pulled in) — note the new `Sketch uses N bytes` percentage. If overflow against the default partition: stop here and decide between (a) trimming a gallery style (edit `tools/gen_eye_gallery_bundles.py` `SPECS`), or (b) switching `PartitionScheme=huge_app` in the FQBN. Document the chosen mitigation in the commit message.

- [ ] **Step 4.3: Hardware probe (one board)**

Flash + monitor. Expected serial includes:
```
eye_sync: init ok ch=1 mac=AA:BB:CC:DD:EE:FF
```

(MAC will be your board's actual MAC.) **Record both boards' MAC addresses** — useful for reading later RX logs.

If `init FAILED` appears: ESP-NOW stack is mis-configured. Most common cause is calling `esp_now_init()` before `WiFi.mode(WIFI_STA)`, which we don't, but verify the order.

- [ ] **Step 4.4: Commit**

```bash
git add eye_sync.cpp
git commit -m "feat(sync): real WiFi STA + ESP-NOW init + broadcast peer"
```

---

### Task 5: Heartbeat + tap broadcast (TX path)

**Goal:** Both boards emit a heartbeat every 2 s and emit an immediate "tap-triggered" message via `eye_sync_broadcast_index`. The wire is now noisy. Receive logic still ignores everything.

**Files:**
- Modify: `eye_sync.cpp`

- [ ] **Step 5.1: Add a static helper to build + send a message**

Just **above** the `eye_sync_init()` definition in `eye_sync.cpp`, add:

```c
static void send_msg(uint8_t index, uint8_t flags) {
  EyeSyncMsg msg;
  msg.magic[0] = EYE_SYNC_MAGIC0;
  msg.magic[1] = EYE_SYNC_MAGIC1;
  msg.magic[2] = EYE_SYNC_MAGIC2;
  msg.magic[3] = EYE_SYNC_MAGIC3;
  msg.msg_type = EYE_SYNC_TYPE_GALLERY;
  msg.index    = index;
  msg.flags    = flags;
  msg.reserved = 0;

  esp_err_t r = esp_now_send(s_broadcast_addr,
                             (const uint8_t*)&msg, sizeof(msg));
#if EYE_SYNC_LOG
  Serial.printf("eye_sync: tx idx=%u flag=%s rc=%d\n",
                (unsigned)index,
                (flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb",
                (int)r);
#else
  (void)r;
#endif
}
```

- [ ] **Step 5.2: Replace the empty `eye_sync_tick()` body with heartbeat scheduling**

Locate the existing `eye_sync_tick` body that early-returns when `!s_inited`. Replace it with:

```c
void eye_sync_tick(void) {
  if (!s_inited) {
    return;
  }
  // RX drain — filled in Task 6.

  uint32_t now = millis();
  if ((uint32_t)(now - s_last_heartbeat_ms) >= EYE_SYNC_HEARTBEAT_MS) {
    send_msg(s_local_index, /*flags=*/0);
    s_last_heartbeat_ms = now;
  }
}
```

- [ ] **Step 5.3: Replace the empty `eye_sync_broadcast_index()` body**

```c
void eye_sync_broadcast_index(uint8_t idx) {
  if (!s_inited) {
    return;
  }
  uint32_t now            = millis();
  s_local_index           = idx;
  s_last_local_change_ms  = now;
  s_last_heartbeat_ms     = now;  // suppress immediate redundant heartbeat
  send_msg(idx, EYE_SYNC_FLAG_TAP);
}
```

- [ ] **Step 5.4: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile.

- [ ] **Step 5.5: Hardware probe (one board)**

Flash + monitor. Expected serial: every ~2 s a line like
```
eye_sync: tx idx=0 flag=hb rc=0
```

`rc=0` means `ESP_OK`. Any other value means the send call rejected — most likely peer not added or interface down.

We are **not** wiring `eye_sync_broadcast_index` from the gallery yet — that's Task 8. Touch on the panel still advances the local gallery, but no `flag=tap` line will appear.

- [ ] **Step 5.6: Commit**

```bash
git add eye_sync.cpp
git commit -m "feat(sync): heartbeat + tap broadcast (TX path)"
```

---

### Task 6: Receive callback + ring buffer + drain in tick

**Goal:** Boards observe each other's traffic. RX callback queues into a small ring; `eye_sync_tick()` drains and logs each message. **No state changes yet** — apply logic comes in Task 9.

**Files:**
- Modify: `eye_sync.cpp`

- [ ] **Step 6.1: Add ring buffer + RX callback near the top of the `#if EYE_SYNC_ENABLE` block**

Place this **after** the static state variables (`s_inited`, `s_local_index`, ...) and **before** the `send_msg` helper:

```c
// 4-slot single-producer / single-consumer ring buffer. Producer is the
// ESP-NOW receive callback (WiFi task context); consumer is eye_sync_tick()
// on the main loop. uint8_t indices are atomic on Xtensa, so no lock.
#define EYE_SYNC_RX_QSIZE 4u
static volatile EyeSyncMsg s_rx_queue[EYE_SYNC_RX_QSIZE];
static volatile uint8_t    s_rx_head = 0;  // written by callback
static volatile uint8_t    s_rx_tail = 0;  // written by tick

// MAC of the most recent sender, captured at enqueue time. Indexed by
// the queue slot. Used only for log output.
static volatile uint8_t s_rx_mac[EYE_SYNC_RX_QSIZE][6];

static void on_recv_cb(const esp_now_recv_info_t* info,
                       const uint8_t* data, int len) {
  if (len != (int)sizeof(EyeSyncMsg)) {
    return;
  }
  uint8_t next = (uint8_t)((s_rx_head + 1u) % EYE_SYNC_RX_QSIZE);
  if (next == s_rx_tail) {
    // queue full — drop. We'll resync on the next heartbeat.
    return;
  }
  memcpy((void*)&s_rx_queue[s_rx_head], data, sizeof(EyeSyncMsg));
  if (info != nullptr) {
    memcpy((void*)s_rx_mac[s_rx_head], info->src_addr, 6);
  } else {
    memset((void*)s_rx_mac[s_rx_head], 0, 6);
  }
  s_rx_head = next;
}
```

- [ ] **Step 6.2: Register the RX callback inside `eye_sync_init()`**

In `eye_sync_init()`, immediately **after** the successful `esp_now_init()` call (and before `esp_now_add_peer`), add:

```c
  esp_now_register_recv_cb(on_recv_cb);
```

So the relevant section reads:

```c
  if (esp_now_init() != ESP_OK) {
#if EYE_SYNC_LOG
    Serial.println("eye_sync: esp_now_init FAILED");
#endif
    return;
  }

  esp_now_register_recv_cb(on_recv_cb);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
```

- [ ] **Step 6.3: Drain the RX ring at the start of `eye_sync_tick()`**

Replace the `// RX drain — filled in Task 6.` comment with:

```c
  while (s_rx_tail != s_rx_head) {
    EyeSyncMsg m;
    uint8_t    mac[6];
    memcpy(&m,  (const void*)&s_rx_queue[s_rx_tail], sizeof(EyeSyncMsg));
    memcpy(mac, (const void*)s_rx_mac[s_rx_tail],     6);
    s_rx_tail = (uint8_t)((s_rx_tail + 1u) % EYE_SYNC_RX_QSIZE);

    // Drop foreign or wrong-type traffic up front.
    if (m.magic[0] != EYE_SYNC_MAGIC0 || m.magic[1] != EYE_SYNC_MAGIC1 ||
        m.magic[2] != EYE_SYNC_MAGIC2 || m.magic[3] != EYE_SYNC_MAGIC3) {
      continue;
    }
    if (m.msg_type != EYE_SYNC_TYPE_GALLERY) {
      continue;  // reserved for phase B
    }

#if EYE_SYNC_LOG
    Serial.printf("eye_sync: rx idx=%u from=%02X:%02X:%02X:%02X:%02X:%02X flag=%s\n",
                  (unsigned)m.index,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (m.flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb");
#endif

    // Apply logic added in Task 9.
  }
```

- [ ] **Step 6.4: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile.

- [ ] **Step 6.5: Hardware probe — TWO BOARDS**

Flash this firmware to **both** boards. Power both and open serial monitors on each (two terminals).

Expected on **both** boards:
- Their own `tx idx=0 flag=hb` every ~2 s.
- The other board's heartbeats logged as `rx idx=0 from=<other-mac> flag=hb` within the same window.

If only one direction works: usually a channel mismatch. Confirm both boards report `ch=1` (or whatever you set in `config.h`).

If neither direction works: confirm both boards' ESP-NOW init logged `init ok` and not `FAILED`.

- [ ] **Step 6.6: Commit**

```bash
git add eye_sync.cpp
git commit -m "feat(sync): RX callback + ring buffer + log-only drain"
```

---

### Task 7: `eye_gallery_apply_remote_index` — non-broadcasting state setter

**Goal:** New gallery function that sets the active style **without** triggering a broadcast. Not yet called from anywhere; just compiles.

**Files:**
- Modify: `eye_gallery.h`
- Modify: `eye_gallery.cpp`

- [ ] **Step 7.1: Add prototype to `eye_gallery.h`**

In [`eye_gallery.h`](../../../eye_gallery.h), add **between** `void eye_gallery_next(void);` and `void eye_gallery_poll(void);`:

```c
/** Set the active gallery to the given index WITHOUT broadcasting.
 *  Used by the sync receive path to avoid re-triggering a network round trip. */
void eye_gallery_apply_remote_index(uint8_t idx);
```

- [ ] **Step 7.2: Add definition in `eye_gallery.cpp`**

In [`eye_gallery.cpp`](../../../eye_gallery.cpp), immediately **after** the existing `eye_gallery_next()` definition (around line 104), add:

```c
void eye_gallery_apply_remote_index(uint8_t idx) {
  if (idx >= EYE_GALLERY_NUM) {
    return;  // ignore garbage from the wire
  }
  if ((size_t)idx == s_gallery_idx) {
    return;  // already in sync
  }
  s_gallery_idx = (size_t)idx;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: <- ");  // arrow distinguishes remote from local "->"
  Serial.println(eye_gallery[s_gallery_idx].name);
}
```

- [ ] **Step 7.3: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile. The new function is unused — that's fine, expected `-Wunused` is not enabled in Arduino default flags.

- [ ] **Step 7.4: Commit**

```bash
git add eye_gallery.h eye_gallery.cpp
git commit -m "feat(sync): non-broadcasting eye_gallery_apply_remote_index"
```

---

### Task 8: Wire local tap → broadcast

**Goal:** Tapping (or `n` over serial) advances the local gallery **and** broadcasts. The peer board logs the message but still does not apply it.

**Files:**
- Modify: `eye_gallery.cpp`

- [ ] **Step 8.1: Include the sync header in `eye_gallery.cpp`**

At the top of [`eye_gallery.cpp`](../../../eye_gallery.cpp), with the other includes (alongside `#include "eye_gallery.h"`):

```c
#include "eye_sync.h"
```

- [ ] **Step 8.2: Add the broadcast call to `eye_gallery_next()`**

Locate `void eye_gallery_next(void)`:

```c
void eye_gallery_next(void) {
  s_gallery_idx = (s_gallery_idx + 1) % EYE_GALLERY_NUM;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: -> ");
  Serial.println(eye_gallery[s_gallery_idx].name);
}
```

Append, immediately **before** the closing `}`:

```c
#if EYE_SYNC_ENABLE
  eye_sync_broadcast_index((uint8_t)s_gallery_idx);
#endif
```

So the function becomes:

```c
void eye_gallery_next(void) {
  s_gallery_idx = (s_gallery_idx + 1) % EYE_GALLERY_NUM;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: -> ");
  Serial.println(eye_gallery[s_gallery_idx].name);
#if EYE_SYNC_ENABLE
  eye_sync_broadcast_index((uint8_t)s_gallery_idx);
#endif
}
```

- [ ] **Step 8.3: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile.

- [ ] **Step 8.4: Hardware probe — TWO BOARDS**

Flash both boards. On board A's monitor, send `n\n` (or tap the screen). Expected:

On A:
```
eye_gallery: -> owl
eye_sync: tx idx=1 flag=tap rc=0
```

On B:
```
eye_sync: rx idx=1 from=<A-mac> flag=tap
```

B's gallery does **not** change yet — that's Task 9. Indices on A and B are now diverged. Heartbeats from A continue showing `idx=1` and from B continue showing `idx=0`; that's expected diagnostic output.

- [ ] **Step 8.5: Commit**

```bash
git add eye_gallery.cpp
git commit -m "feat(sync): broadcast local gallery advance"
```

---

### Task 9: Wire RX → apply with race guard (the actual sync)

**Goal:** Receive path applies the remote index. Tap on either board cycles both. Asymmetric boot recovers via heartbeat.

**Files:**
- Modify: `eye_sync.cpp`

- [ ] **Step 9.1: Replace the `// Apply logic added in Task 9.` comment**

In `eye_sync.cpp`, inside `eye_sync_tick()`, replace the trailing `// Apply logic added in Task 9.` line at the end of the drain loop with the apply block:

```c
    // Drop in-sync messages cheaply.
    if (m.index == s_local_index) {
      continue;
    }

    // Race guard: if we just tapped locally, our outbound message is in
    // flight and the peer's heartbeat may carry the OLD index. Suppress
    // applying inbound for EYE_SYNC_RACE_GUARD_MS after a local tap.
    uint32_t since_local = (uint32_t)(millis() - s_last_local_change_ms);
    if (since_local < EYE_SYNC_RACE_GUARD_MS) {
#if EYE_SYNC_LOG
      Serial.println("eye_sync:   ignore (race-guard)");
#endif
      continue;
    }

    eye_gallery_apply_remote_index(m.index);
    s_local_index = m.index;
```

The full inside-loop block now reads (for reference — do **not** duplicate, just confirm):

```c
    if (m.magic[0] != EYE_SYNC_MAGIC0 || m.magic[1] != EYE_SYNC_MAGIC1 ||
        m.magic[2] != EYE_SYNC_MAGIC2 || m.magic[3] != EYE_SYNC_MAGIC3) {
      continue;
    }
    if (m.msg_type != EYE_SYNC_TYPE_GALLERY) {
      continue;
    }

#if EYE_SYNC_LOG
    Serial.printf("eye_sync: rx idx=%u from=%02X:%02X:%02X:%02X:%02X:%02X flag=%s\n",
                  (unsigned)m.index,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (m.flags & EYE_SYNC_FLAG_TAP) ? "tap" : "hb");
#endif

    if (m.index == s_local_index) {
      continue;
    }

    uint32_t since_local = (uint32_t)(millis() - s_last_local_change_ms);
    if (since_local < EYE_SYNC_RACE_GUARD_MS) {
#if EYE_SYNC_LOG
      Serial.println("eye_sync:   ignore (race-guard)");
#endif
      continue;
    }

    eye_gallery_apply_remote_index(m.index);
    s_local_index = m.index;
```

- [ ] **Step 9.2: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile.

- [ ] **Step 9.3: Hardware probe — TWO BOARDS, full acceptance scenarios**

Flash both. Run through this list and confirm each:

| Scenario | Expected |
|----------|----------|
| Both fresh boot, no tap | Both on `nauga`. Both log heartbeat tx every 2 s. Each logs the other's heartbeat as rx. No `apply` lines (in-sync drop). |
| Tap board A | A: `eye_gallery: -> owl` + `tx idx=1 flag=tap`. B: `rx idx=1 ... flag=tap` + `eye_gallery: <- owl`. Both panels visibly show owl within < 100 ms. |
| Tap board B | Symmetric: A: `<- cat` (etc.). |
| Boot A, tap A 3× (advance to index 3), THEN power on B | B boots on `nauga` (idx 0), then within ≤ 3 s logs `rx idx=3 ... flag=hb` + `<- goat`. Panels are in sync. |
| Reset B (unplug + replug) while A is running | After B's WiFi init + first received heartbeat (≤ 3 s), B is back in sync. |
| Tap A immediately followed by tap B (within 500 ms) | B's tap happens during A's race-guard window; B's broadcast may be ignored by A. Both end up showing **A's** index. **Or** vice versa. Important: they end up consistent within the next heartbeat (≤ 2 s). Briefly diverging is acceptable per spec. |

If any of the first four scenarios fail, debug before continuing. The fifth (simultaneous tap) is best-effort per spec.

**FPS sanity check:** Watch one board's `FPS=` line (logged every 256 frames) over ~30 s. Compare to a baseline measurement on the same board with `EYE_SYNC_ENABLE 0`. Spec acceptance criterion is regression ≤ 1 FPS. If the gap is larger, check whether `eye_sync_tick()` is somehow doing per-frame work it shouldn't (it should be a timestamp compare + empty queue most ticks).

- [ ] **Step 9.4: Commit**

```bash
git add eye_sync.cpp
git commit -m "feat(sync): apply remote index with race guard"
```

---

### Task 10: Verify `EYE_SYNC_ENABLE 0` fallback build

**Goal:** Single-eye fallback compiles cleanly and links no WiFi/ESP-NOW code. Binary noticeably smaller; runtime behavior identical to current main-branch firmware.

**Files:**
- Modify: `config.h` (temporarily)

- [ ] **Step 10.1: Flip the flag**

In `config.h`, change:

```c
#define EYE_SYNC_ENABLE         1
```

to:

```c
#define EYE_SYNC_ENABLE         0
```

- [ ] **Step 10.2: Compile probe**

```bash
arduino-cli compile --fqbn "$FQBN" -v . 2>&1 | tail -n 30
```

Expected:
- Clean compile.
- "Sketch uses N bytes" — record this number.
- Compare to the post-Task-4 size (when WiFi was first linked). The fallback build should be **noticeably smaller** (typically 80–120 KB less). If the size is similar, the `#if EYE_SYNC_ENABLE` guards are not actually excluding the WiFi headers — re-check `eye_sync.cpp`.

- [ ] **Step 10.3: Hardware probe (one board)**

Flash. Expected serial — **none** of the `eye_sync:` lines should appear:
```
uncanny-eyes: boot
initEyes: runtime gallery v1
eye_gallery: start nauga
uncanny-eyes: display_begin()
qspi_async: init ok
eye_gallery: touch ok CST9217
uncanny-eyes: running
FPS=...
```

Tap on the panel: gallery advances locally as before (no `tx` line, no broadcast).

- [ ] **Step 10.4: Restore the flag**

```c
#define EYE_SYNC_ENABLE         1
```

- [ ] **Step 10.5: Compile probe (back to sync build)**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

Expected: clean compile, original (larger) binary size.

- [ ] **Step 10.6: Commit**

The flag value is the only diff and we're back to `1`, so there's nothing to commit. Skip the commit step. (If you want a record of the size measurements, add a short note to the README in Task 11.)

---

### Task 11: README + spec status update

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md`

- [ ] **Step 11.1: Add a "Pair sync" section to `README.md`**

Insert this **before** the "Known limitations / ideas for later" heading:

```markdown
## Pair sync (phase C — gallery index over ESP-NOW)

When two boards are running this firmware on the same WiFi channel, a tap on
either board cycles **both** to the next gallery style. Sync uses ESP-NOW
broadcast with a 4-byte magic prefix — no router, no AP, no extra library.

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

Tap on either board produces `flag=tap` on the sender and an arrow log
(`eye_gallery: <- owl`) on the receiver. Asymmetric boot self-heals on the
next heartbeat (≤ 2 s).

Animation sync (eyes looking at the same point, blinking together) is **phase
B**, not in this build.
```

- [ ] **Step 11.2: Update the "One eye only" bullet in "Known limitations / ideas for later"**

Find the existing bullet (around [`README.md:147`](../../../README.md#L147)):

```markdown
- **One eye only.** Two boards would need sync (e.g. ESP-NOW exchanging a shared RNG seed) so both eyes look at the same thing.
```

Replace with:

```markdown
- **Animation sync (phase B).** Phase C (this build) keeps both boards on the same gallery style; both still animate independently. Phase B will sync eye motion / blink / iris, likely via a shared RNG seed broadcast on the same ESP-NOW transport.
```

- [ ] **Step 11.3: Mark the spec as implemented**

Open [`docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md`](../specs/2026-04-28-eye-sync-phase-c-design.md). Change the header line:

```
**Status:** design, not yet implemented
```

to:

```
**Status:** implemented (branch `feat/eye-sync-phase-c`)
```

- [ ] **Step 11.4: Compile probe (sanity)**

```bash
arduino-cli compile --fqbn "$FQBN" .
```

- [ ] **Step 11.5: Commit**

```bash
git add README.md docs/superpowers/specs/2026-04-28-eye-sync-phase-c-design.md
git commit -m "docs(sync): README pair-sync section + mark spec implemented"
```

---

### Task 12: Land the branch

**Files:** none

- [ ] **Step 12.1: Container merge per [`AGENTS.md`](../../../AGENTS.md)**

```bash
git checkout main
git merge --no-ff feat/eye-sync-phase-c
git branch -d feat/eye-sync-phase-c
```

Do not push without confirming with the user.
