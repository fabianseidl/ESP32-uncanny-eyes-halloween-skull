# v2b Async QSPI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the v2a 10 fps QSPI bottleneck to ≥ 20 fps (target 30) by pipelining pixel DMA with CPU byte-swap/compute via a second `spi_device_handle_t` owned in app-space, while the `Arduino_GFX` library stays byte-identical to an installed Library-Manager copy.

**Architecture:** `Arduino_ESP32QSPI` is instantiated with `is_shared_interface=true` so the library releases the bus between its own operations (`beginWrite`/`endWrite`). A new `display_async.cpp` module opens a second `spi_device_handle_t` on `SPI2_HOST` with `queue_size=3`, owns two `MALLOC_CAP_DMA` ping-pong buffers, byte-swaps each 1024-px chunk into the next slot and queues it with `spi_device_queue_trans()` without blocking, waits on `spi_device_get_trans_result()` only when the slot is needed again. `drawEye()` becomes two phases: a cold phase through the library (`setAddrWindow` via handle-L), then a hot phase (`display_pixelsBegin` … `display_pixelsQueueChunk` ×N … `display_pixelsEnd` via handle-A). The renderer's per-frame ping-pong and `yield()` go away.

**Tech Stack:** Arduino IDE + arduino-cli, Arduino_GFX (`Arduino_CO5300` + `Arduino_ESP32QSPI`), ESP-IDF `driver/spi_master.h` (app-level — we only *add* a device to the already-initialized bus), ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.75, CO5300 QSPI 466×466 @ 40 MHz), C++17.

**Verification model:** No host-side unit tests — embedded code bound to the panel + library. Verification is (a) clean `arduino-cli compile`, (b) flash-and-look (color/motion/blink match v2a visually), (c) serial `FPS=<n>` log ≥ 20, (d) `sha256sum` of library files before and after to confirm no library mutation, (e) `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)` at end of `setup()` ≥ 50 KB.

**Spec:** [`docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md`](../specs/2026-04-19-v2b-async-qspi-design.md)

**Branch:** `feat/v2b-async-qspi` (already created, per AGENTS.md convention).

---

## File Structure

Files created or modified, with responsibility:

- **`config.h`** — Add one constant: `QSPI_ASYNC_CHUNK_PX` (must equal library's `ESP32QSPI_MAX_PIXELS_AT_ONCE`, default 1024, because our transaction format assumes the library's per-chunk framing). No other changes. Keeps all compile-time knobs in one file.
- **`display.ino`** — Constructor-arg flip to `is_shared_interface=true`. Calls `display_async_init()` after `s_gfx->begin()`. Removes `display_writePixels()` (no surviving callers). All Arduino_GFX calls stay here.
- **`display_async.cpp`** — **New.** Owns handle-A: `spi_bus_add_device()`, two 16-byte-aligned `MALLOC_CAP_DMA` buffers, two `spi_transaction_ext_t`, the ping-pong state machine. Exposes `display_async_init / display_pixelsBegin / display_pixelsQueueChunk / display_pixelsEnd`. All transaction framing (`cmd=0x32 addr=0x003C00` on first chunk, continuation otherwise) transcribed directly from `Arduino_ESP32QSPI.cpp:322–367` with a source-line citation comment.
- **`display_async.h`** — **New.** Thin C-callable declarations for the four functions above. Included by `display.ino` and `eye_functions.ino`.
- **`ESP32-uncanny-eyes-halloween-skull.ino`** — Remove `pbuffer[BUFFERS][BUFFER_SIZE]`, `dmaBuf`, `#define BUFFER_SIZE`, `#define BUFFERS`. Update top-of-file comment to cite the v2b spec.
- **`eye_functions.ino`** — Rewrite `emitRow()` / `emitRowFlushTail()` against `display_pixelsQueueChunk`. Split `drawEye()` cold-phase (library holds bus for `setAddrWindow`) / hot-phase (handle-A holds bus for pixel stream). Remove `s_emitPixels` globalized state; replace with file-scope `s_chunk_buf[QSPI_ASYNC_CHUNK_PX]` + `s_chunk_fill`. Remove `yield()` in the flush path. `drawEyeRow()`, `expandRow()`, horizontal/vertical Bresenham, `frame()`, `updateEye()`, `split()`, `initEyes()` — **bit-for-bit unchanged**.
- **`README.md`** — Targeted edits: note async-QSPI implementation, refresh FPS number after measurement, mark dirty-rect as deferred.

**Task decomposition: five tasks.** Each leaves the tree compiling and, where applicable, flashes to a viable image.

- **Task 1** adds the async module as *dead code* compiled but not called. Library still drives pixels via `display_writePixels`. Proves the module compiles, links, and device creation succeeds (checked at boot).
- **Task 2** routes the renderer's pixel stream through the async module while **still** keeping the library pixel API callable and the bus-share flag `false`. Functionally a drop-in: library holds bus forever (as before), we never invoke library `writePixels` after init. This isolates "renderer talks to new module" from "bus sharing works".

  > ⚠️ Task 2 introduces bus contention — `display_pixelsBegin()` will call `spi_device_acquire_bus(handle_A)` while handle-L holds it from `begin()`. We expect this to hang. Task 2 therefore **does not flash**. Compile only; flash is in Task 3. This is intentional: Task 2 is "renderer rewire" and Task 3 is "bus-share flip", reviewable as independent diffs.

- **Task 3** flips `is_shared_interface=true`, splits `drawEye()` into cold/hot phases with proper `startWrite`/`endWrite` bracketing around `setAddrWindow`. First frame appears. This is the user-visible "v2b works" commit.
- **Task 4** removes dead code (`display_writePixels`, `pbuffer[][]`, `dmaBuf`, `BUFFER_SIZE`, `BUFFERS`, `s_emitPixels`) now that nothing references it.
- **Task 5** runs the verification matrix (both eye sides, FPS measurement, library sha256 check, README wording) and commits the README.

---

## Task 1: Add `display_async` module as compiled-but-unused dead code

Create the async module and its header, wire `display_async_init()` into `display_begin()`. The renderer still pushes pixels through the library; the new device exists, its buffers allocate, and it reports `qspi_async: init ok` on the serial console. No pixel data flows through it yet.

**Files:**
- Create: `display_async.h`
- Create: `display_async.cpp`
- Modify: `display.ino`

- [ ] **Step 1.1: Create `display_async.h` with the C API.**

Create `display_async.h`:

```c
// display_async.h -- async QSPI pixel pusher on a second spi_device_handle_t.
// See docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md for design
// and docs/superpowers/plans/2026-04-19-v2b-async-qspi.md for rollout.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create handle-A on SPI2_HOST and allocate both DMA ping-pong slots.
// Must be called AFTER s_gfx->begin() (which owns spi_bus_initialize).
// On failure, logs and halts -- same discipline as display_begin().
void display_async_init(void);

// Acquire bus on handle-A, pull CS low, reset chunk state.
// First chunk after this will emit the CO5300 RAMWR framing
// (cmd=0x32, addr=0x003C00).
void display_pixelsBegin(void);

// Byte-swap `len` RGB565 pixels (host endian -> big endian) into the
// next DMA slot and queue the transfer non-blocking. If the slot was
// still in flight from a previous call, block on its completion first.
// `len` MUST be <= QSPI_ASYNC_CHUNK_PX.
void display_pixelsQueueChunk(const uint16_t *px, uint32_t len);

// Drain both DMA slots, raise CS, release bus on handle-A.
void display_pixelsEnd(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 1.2: Create `display_async.cpp` with device init + state-machine stubs.**

Create `display_async.cpp`:

```cpp
// display_async.cpp -- async QSPI pixel pusher. See display_async.h and
// docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md.
//
// Transaction-framing values (cmd=0x32, addr=0x003C00, QIO flags) are
// transcribed from Arduino_ESP32QSPI.cpp's writePixels() at installed
// library source ~/Documents/Arduino/libraries/GFX_Library_for_Arduino/
// src/databus/Arduino_ESP32QSPI.cpp:322-367. If that library file
// changes upstream, re-verify those values before upgrading.

#include "display_async.h"
#include "config.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_err.h>

// Pin map duplicated from display.ino -- kept local here so this file is
// self-contained for future extraction. See docs/hardware-notes.md.
#define QSPI_ASYNC_CS    12
#define QSPI_ASYNC_SCLK  38
#define QSPI_ASYNC_D0    4
#define QSPI_ASYNC_D1    5
#define QSPI_ASYNC_D2    6
#define QSPI_ASYNC_D3    7

// Must match library's ESP32QSPI_FREQUENCY + ESP32QSPI_SPI_MODE +
// ESP32QSPI_SPI_HOST. We depend on the library having already called
// spi_bus_initialize() on SPI2_HOST at this clock / mode.
#define QSPI_ASYNC_CLOCK_HZ 40000000
#define QSPI_ASYNC_HOST     SPI2_HOST

static spi_device_handle_t   s_handle;
static uint16_t             *s_dma_buf[2];
static spi_transaction_ext_t s_trans[2];
static uint8_t               s_buf_idx;
static bool                  s_first_chunk;
static bool                  s_inflight[2];

// Direct-GPIO CS control -- same scheme as library (_csPortSet / _csPortClr).
// CS=12 on ESP32-S3 lives in the low bank (< 32), so we hit GPIO_OUT_W1T*.
static inline void cs_high(void) { *(volatile uint32_t *)GPIO_OUT_W1TS_REG = (1u << QSPI_ASYNC_CS); }
static inline void cs_low(void)  { *(volatile uint32_t *)GPIO_OUT_W1TC_REG = (1u << QSPI_ASYNC_CS); }

void display_async_init(void) {
  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits    = 8;
  devcfg.address_bits    = 24;
  devcfg.dummy_bits      = 0;
  devcfg.mode            = 0;                     // SPI_MODE0, matches library.
  devcfg.duty_cycle_pos  = 0;
  devcfg.cs_ena_pretrans = 0;
  devcfg.cs_ena_posttrans = 0;
  devcfg.clock_speed_hz  = QSPI_ASYNC_CLOCK_HZ;
  devcfg.input_delay_ns  = 0;
  devcfg.spics_io_num    = -1;                    // Manual CS -- same as library.
  devcfg.flags           = SPI_DEVICE_HALFDUPLEX;
  devcfg.queue_size      = 3;                     // One slot of headroom over our 2-queued ceiling.

  esp_err_t ret = spi_bus_add_device(QSPI_ASYNC_HOST, &devcfg, &s_handle);
  if (ret != ESP_OK) {
    Serial.print("qspi_async: spi_bus_add_device FAILED rc=");
    Serial.println(ret);
    while (true) { delay(1000); }
  }

  for (int i = 0; i < 2; i++) {
    s_dma_buf[i] = (uint16_t *)heap_caps_aligned_alloc(
        16, QSPI_ASYNC_CHUNK_PX * 2, MALLOC_CAP_DMA);
    if (!s_dma_buf[i]) {
      Serial.println("qspi_async: heap_caps_aligned_alloc FAILED");
      while (true) { delay(1000); }
    }
    s_inflight[i] = false;
    memset(&s_trans[i], 0, sizeof(s_trans[i]));
  }
  s_buf_idx     = 0;
  s_first_chunk = true;

  Serial.println("qspi_async: init ok");
}

void display_pixelsBegin(void) {
  // Task 3 replaces the body of this function with the real acquire/CS logic.
  // Keeping it a noop in Task 1 means nothing calls through yet.
}

void display_pixelsQueueChunk(const uint16_t *px, uint32_t len) {
  (void)px; (void)len;
  // Task 2 fills this in.
}

void display_pixelsEnd(void) {
  // Task 3 fills this in.
}
```

- [ ] **Step 1.3: Wire `display_async_init()` into `display_begin()`.**

Edit `display.ino`. Add include at top with the other includes:

```cpp
#include "display_async.h"
```

At the end of `display_begin()` (after the successful `s_gfx->begin()` branch), add:

```cpp
  display_async_init();
```

- [ ] **Step 1.4: Verify clean compile.**

Run from repo root:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: `Sketch uses` line, zero errors, zero new warnings beyond v2a baseline.

If the FQBN differs from your normal invocation, use whichever FQBN produced the v2a `FPS=10` baseline. Check shell history / `tools/` for the canonical flash command.

- [ ] **Step 1.5: Flash and confirm the new serial line.**

Flash to the board. Serial log should now show:

```
uncanny-eyes: boot
initEyes: single eye v1
uncanny-eyes: display_begin()
qspi_async: init ok
uncanny-eyes: running
FPS=10
```

The FPS stays at v2a baseline — async module is still unused. Critical check: `qspi_async: init ok` appears **and** the subsequent `FPS=10` lines mean `spi_bus_add_device` succeeded without starving the library's handle.

- [ ] **Step 1.6: Commit.**

```bash
git add display_async.h display_async.cpp display.ino
git commit -m "$(cat <<'EOF'
feat(display): add async QSPI module skeleton (unused)

Creates handle-A on SPI2_HOST via spi_bus_add_device plus two
MALLOC_CAP_DMA ping-pong slots. Module is compiled and initialized
but not yet invoked -- renderer still pushes pixels via the library.

Task 1 of v2b per docs/superpowers/plans/2026-04-19-v2b-async-qspi.md.
EOF
)"
```

---

## Task 2: Route renderer pixels through async module (compile-only)

Fill in `display_pixelsQueueChunk`'s byte-swap + queue state machine. Rewrite `emitRow` / `emitRowFlushTail` to drive it instead of `display_writePixels`. Stub `display_pixelsBegin` / `display_pixelsEnd` to *only* manage CS + chunk state (no bus acquire yet — Task 3 adds that). Renderer now points at the async module end-to-end, but since `is_shared_interface` is still `false`, handle-L is holding the bus forever — if we flashed this, `spi_device_queue_trans(s_handle, ...)` would hang.

> ⚠️ **Do not flash at the end of this task.** Compile only. Task 3 flips the bus-share flag; that's when the board next sees live firmware. This sequencing keeps "renderer rewire" and "bus-share coordination" as independently diffable commits.

**Files:**
- Modify: `config.h`
- Modify: `display_async.cpp`
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino`
- Modify: `eye_functions.ino`

- [ ] **Step 2.1: Add `QSPI_ASYNC_CHUNK_PX` to `config.h`.**

Edit `config.h`. Append (anywhere after `#pragma once`, just before the `IRIS_MIN` block is fine):

```c
// Chunk size for async QSPI pixel pushes. Must equal ESP32QSPI_MAX_PIXELS_AT_ONCE
// (1024 default) since our transaction format (cmd=0x32 addr=0x003C00 on first
// chunk, continuation otherwise) assumes the library's per-chunk framing.
#define QSPI_ASYNC_CHUNK_PX 1024
```

- [ ] **Step 2.2: Add the renderer-side chunk accumulator to `eye_functions.ino`.**

Edit `eye_functions.ino`. Just after the `extern uint16_t line_dst[];` block, add:

```c
#include "display_async.h"

// Renderer-side accumulator. Each emitted row is appended; when it reaches
// QSPI_ASYNC_CHUNK_PX we hand the slice off to display_pixelsQueueChunk.
// Replaces v2a's pbuffer[2][BUFFER_SIZE] ping-pong (the async module owns
// its own DMA ping-pong internally).
static uint16_t s_chunk_buf[QSPI_ASYNC_CHUNK_PX];
static uint32_t s_chunk_fill = 0;
```

Leave the old `s_emitPixels` static declaration in place for now — Task 4 removes it. The new accumulator is just added alongside; no collision.

- [ ] **Step 2.3: Replace `emitRow` and `emitRowFlushTail` bodies.**

Edit `eye_functions.ino`. Replace the existing `emitRow` body (currently referencing `pbuffer`, `dmaBuf`, `s_emitPixels`, `display_writePixels`, `yield`) with:

```c
static void emitRow(const uint16_t* dst) {
  for (uint32_t i = 0; i < RENDER_WIDTH; i++) {
    s_chunk_buf[s_chunk_fill++] = dst[i];
    if (s_chunk_fill == QSPI_ASYNC_CHUNK_PX) {
      display_pixelsQueueChunk(s_chunk_buf, s_chunk_fill);
      s_chunk_fill = 0;
    }
  }
}
```

And replace `emitRowFlushTail` with:

```c
static void emitRowFlushTail() {
  if (s_chunk_fill) {
    display_pixelsQueueChunk(s_chunk_buf, s_chunk_fill);
    s_chunk_fill = 0;
  }
}
```

Critical: no `yield()` in the new `emitRow`. Queued DMA yields via semaphore inside `get_trans_result`; a manual `yield()` in a tight hot loop just adds jitter.

- [ ] **Step 2.4: Fill in `display_pixelsQueueChunk` with the byte-swap + queue state machine.**

Edit `display_async.cpp`. Replace the stub `display_pixelsQueueChunk` body with:

```cpp
void display_pixelsQueueChunk(const uint16_t *px, uint32_t len) {
  const uint8_t slot = s_buf_idx;

  // Wait on the previous transfer into this slot before reusing its buffer.
  if (s_inflight[slot]) {
    spi_transaction_t *done = nullptr;
    esp_err_t rc = spi_device_get_trans_result(s_handle, &done, portMAX_DELAY);
    (void)rc; (void)done;   // portMAX_DELAY + our own driver => effectively cannot fail.
    s_inflight[slot] = false;
  }

  // CPU byte-swap host->BE RGB565 into the DMA-capable slot buffer.
  // Tight loop: compiler lifts to Xtensa byte-reverse on -O2. ~5us / 1024 px.
  uint16_t *dst = s_dma_buf[slot];
  for (uint32_t i = 0; i < len; i++) {
    const uint16_t p = px[i];
    dst[i] = (uint16_t)((p << 8) | (p >> 8));
  }

  // Transcribed from Arduino_ESP32QSPI.cpp writePixels() at lines 333-344
  // of the installed library. Keep in sync with the library if upgraded.
  memset(&s_trans[slot], 0, sizeof(s_trans[slot]));
  if (s_first_chunk) {
    s_trans[slot].base.flags = SPI_TRANS_MODE_QIO;
    s_trans[slot].base.cmd   = 0x32;
    s_trans[slot].base.addr  = 0x003C00;
  } else {
    s_trans[slot].base.flags = SPI_TRANS_MODE_QIO |
                               SPI_TRANS_VARIABLE_CMD |
                               SPI_TRANS_VARIABLE_ADDR |
                               SPI_TRANS_VARIABLE_DUMMY;
    s_trans[slot].command_bits = 0;
    s_trans[slot].address_bits = 0;
    s_trans[slot].dummy_bits   = 0;
  }
  s_trans[slot].base.tx_buffer = dst;
  s_trans[slot].base.length    = len * 16;  // bits.

  esp_err_t rc = spi_device_queue_trans(s_handle, &s_trans[slot].base, portMAX_DELAY);
  if (rc != ESP_OK) {
    Serial.print("qspi_async: queue_trans rc=");
    Serial.println(rc);
  }

  s_inflight[slot] = true;
  s_first_chunk    = false;
  s_buf_idx       ^= 1;
}
```

- [ ] **Step 2.5: Stub `display_pixelsBegin` / `display_pixelsEnd` with CS + state reset (no bus acquire yet).**

Edit `display_async.cpp`. Replace the two stubs:

```cpp
void display_pixelsBegin(void) {
  // Task 3 adds spi_device_acquire_bus(s_handle, portMAX_DELAY) here.
  cs_low();
  s_buf_idx     = 0;
  s_first_chunk = true;
  // s_inflight[] is already {false,false} from the last display_pixelsEnd.
}

void display_pixelsEnd(void) {
  for (int slot = 0; slot < 2; slot++) {
    if (s_inflight[slot]) {
      spi_transaction_t *done = nullptr;
      (void)spi_device_get_trans_result(s_handle, &done, portMAX_DELAY);
      s_inflight[slot] = false;
    }
  }
  cs_high();
  // Task 3 adds spi_device_release_bus(s_handle) here.
}
```

- [ ] **Step 2.6: Wrap the renderer's hot loop in `display_pixelsBegin/End`.**

Edit `eye_functions.ino`, `drawEye()` body. Keep the existing `display_startWrite() / display_setAddrWindow(0,0,RENDER_WIDTH,RENDER_HEIGHT)` line; insert `display_pixelsBegin()` just before the source-row loop and `display_pixelsEnd()` just after `emitRowFlushTail()`:

```c
void drawEye(
    uint32_t iScale,
    uint32_t scleraX,
    uint32_t scleraY,
    uint32_t uT,
    uint32_t lT) {
  display_startWrite();
  display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);
  display_endWrite();

  display_pixelsBegin();

  const uint32_t scleraXsave = scleraX;
  int32_t        irisY       = (int32_t)scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  int32_t vAccum = 0;
  for (uint32_t sy = 0; sy < SCREEN_HEIGHT; sy++) {
    drawEyeRow(sy, scleraXsave, scleraY + sy, irisY + (int32_t)sy,
               iScale, uT, lT);
    expandRow(line_src, line_dst);
    emitRow(line_dst);
    vAccum += (int32_t)RENDER_HEIGHT - (int32_t)SCREEN_HEIGHT;
    while (vAccum >= (int32_t)SCREEN_HEIGHT) {
      vAccum -= SCREEN_HEIGHT;
      emitRow(line_dst);
    }
  }

  emitRowFlushTail();
  display_pixelsEnd();
}
```

Note: the `display_startWrite` / `display_setAddrWindow` / `display_endWrite` trio is already the cold-phase pattern the spec wants. `display_pixelsBegin` / `display_pixelsEnd` bracket the hot phase. At this task's end `display_pixelsBegin` is a CS-and-state-reset only; it does not acquire the bus. Task 3 makes the bus-ownership split real.

`drawEyeRow`, `expandRow`, the Bresenham math, `frame`, `updateEye`, `split`, `initEyes` — untouched.

- [ ] **Step 2.7: Verify clean compile.**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: zero errors, zero new warnings.

**Do not flash.** `is_shared_interface` is still `false`, so handle-L holds the bus exclusively. The first `spi_device_queue_trans(s_handle, ...)` from our handle would block forever (internally waits on the bus semaphore the library is holding).

- [ ] **Step 2.8: Commit.**

```bash
git add config.h display_async.cpp eye_functions.ino
git commit -m "$(cat <<'EOF'
feat(renderer): route pixel stream through async QSPI module

emitRow/emitRowFlushTail feed display_pixelsQueueChunk instead of
display_writePixels. Queue/byte-swap state machine lives in
display_async.cpp; transaction framing transcribed from
Arduino_ESP32QSPI.cpp writePixels() lines 333-344 of the currently
installed library. Renderer ping-pong and yield() are gone; chunk
accumulator is a single 2KB buffer.

NOT yet runnable: is_shared_interface still false, so handle-L holds
the bus and handle-A's queue_trans would hang. Task 3 flips the flag.

Task 2 of v2b per docs/superpowers/plans/2026-04-19-v2b-async-qspi.md.
EOF
)"
```

---

## Task 3: Enable bus sharing; split `drawEye()` cold/hot; first real flash

Flip `is_shared_interface=true` so the library releases the bus between its own ops. Add the real `spi_device_acquire_bus` / `spi_device_release_bus` calls in `display_pixelsBegin` / `display_pixelsEnd`. At this point the pixel stream really does flow through handle-A. First flash that must produce a visible eye.

**Files:**
- Modify: `display.ino`
- Modify: `display_async.cpp`

- [ ] **Step 3.1: Flip the `is_shared_interface` flag in the bus constructor.**

Edit `display.ino`, inside `display_begin()`. The current `Arduino_ESP32QSPI` constructor call takes six pins. Append `true` as the seventh arg:

```cpp
  s_bus = new Arduino_ESP32QSPI(
      PIN_QSPI_CS, PIN_QSPI_SCLK,
      PIN_QSPI_D0, PIN_QSPI_D1, PIN_QSPI_D2, PIN_QSPI_D3,
      /*is_shared_interface=*/true);
```

This matches the library signature at `Arduino_ESP32QSPI.h:28`:

```
Arduino_ESP32QSPI(int8_t cs, int8_t sck, int8_t mosi, int8_t miso,
                  int8_t quadwp, int8_t quadhd, bool is_shared_interface = false);
```

- [ ] **Step 3.2: Add real bus acquire/release in `display_pixelsBegin` / `display_pixelsEnd`.**

Edit `display_async.cpp`. Update the two functions:

```cpp
void display_pixelsBegin(void) {
  esp_err_t rc = spi_device_acquire_bus(s_handle, portMAX_DELAY);
  if (rc != ESP_OK) {
    Serial.print("qspi_async: acquire_bus rc=");
    Serial.println(rc);
  }
  cs_low();
  s_buf_idx     = 0;
  s_first_chunk = true;
}

void display_pixelsEnd(void) {
  for (int slot = 0; slot < 2; slot++) {
    if (s_inflight[slot]) {
      spi_transaction_t *done = nullptr;
      (void)spi_device_get_trans_result(s_handle, &done, portMAX_DELAY);
      s_inflight[slot] = false;
    }
  }
  cs_high();
  spi_device_release_bus(s_handle);
}
```

CS must go **high** only after both slots drain. Any transaction still transferring when CS deasserts would truncate the last row — spec risk #3, #6.

- [ ] **Step 3.3: Verify clean compile.**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: zero errors, zero new warnings.

- [ ] **Step 3.4: Flash and observe.**

Flash to the board. Serial log should show:

```
uncanny-eyes: boot
initEyes: single eye v1
uncanny-eyes: display_begin()
qspi_async: init ok
uncanny-eyes: running
FPS=<n>     # some n, reported every 256 frames
```

Within ~2 s the 466×466 AMOLED should show the eye, visually indistinguishable from v2a: same gaze sweep, same blink, same iris scale, same colors, same lid tracking.

**If the board watchdogs ~5 s after `display_begin()`:** spec risk #1 — `is_shared_interface=true` didn't land. Check `display.ino` diff.

**If colors are red↔blue swapped but shape is coherent:** spec risk #4 — byte-swap direction. The existing loop is `(p << 8) | (p >> 8)`, which is the right direction if v2a's pixels were already BE (and they were; v2a pushed raw RGB565 into `writePixels` which internally did MSB_32_16_16_SET — so the swap above is correct relative to v2a). If you see inversion, confirm the renderer writes host-endian RGB565 into `line_dst` (it does; `drawEyeRow` stores `pgm_read_word` results which in v1/v2a reached `display_writePixels` without an explicit swap).

**If image is shifted, scrolling, or partial:** spec risk #2, #3, #6 — recheck `cmd=0x32 addr=0x003C00` values and the `pixelsEnd` drain order against the transcription in Step 2.4 and this step.

**If FPS is no better than v2a:** spec risk #7 — bump `devcfg.queue_size` in `display_async_init` from 3 to 4, reflash, retest. Leave at the value that works.

- [ ] **Step 3.5: Record the measured FPS for the spec's success-criteria gate.**

Watch the serial log for ~30 s. Record the steady-state `FPS=<n>`. Retain for Task 5's README + measurement commit.

- [ ] **Step 3.6: Commit.**

```bash
git add display.ino display_async.cpp
git commit -m "$(cat <<'EOF'
feat(display): flip bus-share + enable async QSPI pixel stream

Arduino_ESP32QSPI is now constructed with is_shared_interface=true,
so the library acquires/releases the bus per beginWrite/endWrite pair.
display_pixelsBegin/End wrap the handle-A pixel stream in
spi_device_acquire_bus/release_bus. drawEye's cold phase (setAddrWindow
via library) and hot phase (pixel chunks via async module) now hold
the bus in disjoint intervals.

Measured FPS=<n> on target board -- see Task 3 in
docs/superpowers/plans/2026-04-19-v2b-async-qspi.md.
EOF
)"
```

Replace `<n>` in the commit message with the value observed in Step 3.5.

---

## Task 4: Remove v2a dead code

With pixels flowing through the async module, nothing in the tree references `display_writePixels`, `pbuffer`, `dmaBuf`, `BUFFER_SIZE`, `BUFFERS`, or `s_emitPixels`. Delete them.

**Files:**
- Modify: `ESP32-uncanny-eyes-halloween-skull.ino`
- Modify: `display.ino`
- Modify: `eye_functions.ino`

- [ ] **Step 4.1: Remove ping-pong buffers and width macros.**

Edit `ESP32-uncanny-eyes-halloween-skull.ino`. Delete these four lines (+ surrounding comment):

```c
// Ping-pong pixel buffers drained by display_writePixels(). Shared with
// eye_functions.ino.
#define BUFFER_SIZE 1024
#define BUFFERS 2
uint16_t pbuffer[BUFFERS][BUFFER_SIZE];
bool dmaBuf = 0;
```

Replace the top-of-file comment's v2a reference with a v2b one:

```c
// Uncanny Eyes -- Waveshare ESP32-S3-Touch-AMOLED-1.75 port (v2b).
//
// Renders one eye (EYE_SIDE in config.h) full-panel on the 466x466 CO5300
// AMOLED, NN-stretched from the 240-baked asset via a row expander. QSPI
// pixel stream is pipelined through display_async.cpp. See
// docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md.
```

- [ ] **Step 4.2: Remove `display_writePixels` from `display.ino`.**

Edit `display.ino`. Delete the function definition entirely:

```cpp
// Push `len` RGB565 pixels. The renderer already byte-swaps each pixel
// into big-endian form before calling, so writePixels goes out as-is.
void display_writePixels(uint16_t *data, uint32_t len) {
  if (s_gfx) s_gfx->writePixels(data, len);
}
```

No forward declaration lives in a header, so nothing else needs touching.

- [ ] **Step 4.3: Remove `s_emitPixels` from `eye_functions.ino`.**

Edit `eye_functions.ino`. Delete the file-scope declaration + comment:

```c
// Shared with emitRowFlushTail; promoted to file scope so the frame-end
// tail flush can drain the partial DMA buffer.
static uint32_t s_emitPixels = 0;
```

- [ ] **Step 4.4: Verify clean compile.**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: zero errors. If a reference to any of the removed symbols remains somewhere in the tree, the compiler will point it out — delete the reference. No other removals should be needed given the spec's file-change table.

- [ ] **Step 4.5: Flash and confirm nothing regressed.**

Visual match to Task 3.4. Same FPS reading (±1).

- [ ] **Step 4.6: Commit.**

```bash
git add ESP32-uncanny-eyes-halloween-skull.ino display.ino eye_functions.ino
git commit -m "$(cat <<'EOF'
refactor: remove v2a ping-pong + display_writePixels dead code

pbuffer[BUFFERS][BUFFER_SIZE], dmaBuf, BUFFER_SIZE, BUFFERS,
s_emitPixels, display_writePixels -- nothing references any of these
after the async module took over the pixel stream.

Task 4 of v2b per docs/superpowers/plans/2026-04-19-v2b-async-qspi.md.
EOF
)"
```

---

## Task 5: Verify success criteria; update README

Walk the spec's success-criteria list, prove each one on the bench, then commit the README refresh.

**Files:**
- Modify: `README.md` (only at the end, after everything else is verified)

- [ ] **Step 5.1: Verify library integrity — no library files touched.**

Compare the two library files against a freshly-installed copy:

```bash
sha256sum ~/Documents/Arduino/libraries/GFX_Library_for_Arduino/src/databus/Arduino_ESP32QSPI.cpp \
          ~/Documents/Arduino/libraries/GFX_Library_for_Arduino/src/databus/Arduino_ESP32QSPI.h
```

Reinstall the library in a temp dir and compare:

```bash
mkdir -p /tmp/v2b-lib-check && cd /tmp/v2b-lib-check
arduino-cli lib download "GFX Library for Arduino"
# arduino-cli extracts into a versioned dir; unzip the downloaded file if needed.
# Or simpler: arduino-cli lib install --git-url <repo>@<tag> into a throwaway sketchbook.
# Comparison goal: both files byte-identical to the installed copy.
```

Alternatively (faster, same guarantee): verify via `git` that nothing under `~/Documents/Arduino/libraries/` has been modified this session — `ls -l` the two files, their `mtime` should predate this plan's start date.

Expected: **byte-identical**. If not, something in Tasks 1–4 accidentally wrote to the library. Stop and investigate.

- [ ] **Step 5.2: Build both eye sides.**

Edit `config.h`, change `#define EYE_SIDE EYE_SIDE_LEFT` to `EYE_SIDE_RIGHT`, recompile, flash, observe.

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: RIGHT eye renders with correct eyelid-map mirror (caruncle on the correct nose-side). Then revert to `EYE_SIDE_LEFT` and flash once more so the committed default matches repo history.

**Do not commit the flipped-side variant.** The `config.h` edit is a manual bench step.

- [ ] **Step 5.3: Measure sustained FPS.**

Watch serial for ≥ 60 s. Record the steady-state `FPS=<n>`. Apply the spec's gate:

- `n ≥ 30` — target cleared.
- `20 ≤ n ≤ 29` — acceptable; open a follow-up spec for v2c-dirty-rect.
- `n < 20` — not acceptable; open a follow-up spec for v2c-profiling.

The follow-up spec is a separate piece of work, not part of this task. Just record the measurement and note which branch of the gate fires.

- [ ] **Step 5.4: Memory sanity check.**

Add a temporary `Serial.printf("dma_free=%u\n", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));` at the end of `setup()`, reflash, read the value, **then remove the line and reflash again.**

Expected: ≥ 50 KB.

If the temp printf was already committed, revert that file before the next commit.

- [ ] **Step 5.5: Renderer-integrity check.**

```bash
git diff 5d6ef52 -- eye_functions.ino
```

Expected diff scope: only `emitRow`, `emitRowFlushTail`, `drawEye`'s outer structure, the `s_chunk_buf/s_chunk_fill` accumulator, removal of `s_emitPixels`, addition of `display_pixelsBegin/End` calls, and the new `#include "display_async.h"`. `drawEyeRow`, `expandRow`, the Bresenham math, `frame`, `updateEye`, `split`, `initEyes`, eyelid-map mirror logic — all **unchanged** line-for-line.

If any of the untouchable functions shows a diff, stop and reconcile.

- [ ] **Step 5.6: Update `README.md`.**

Targeted edits only. Find the existing paragraphs that describe the v2a render + the measured FPS. Update:

- Description: add "QSPI pixel stream is pipelined through a second `spi_device_handle_t` managed by `display_async.cpp`; library stays unmodified."
- FPS number: replace the v2a `10 fps` figure with the measurement from Step 5.3.
- Roadmap / next-steps: mark dirty-rect as "deferred pending v2b measurement", linking to the v2b spec.

Don't rewrite the README top-to-bottom. Preserve unrelated prose. Keep the "not modified from a freshly-installed copy" stance on the library explicit.

- [ ] **Step 5.7: Final compile check.**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc \
  .
```

Expected: clean. README edits don't affect the sketch, but the check rules out any stray file-level typos that snuck in.

- [ ] **Step 5.8: Commit README.**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
docs(README): note async QSPI pipeline + refreshed FPS

v2b landed: renderer pushes pixels through display_async.cpp
(second spi_device_handle_t on SPI2_HOST), library untouched.
Measured FPS=<n> on target board. Dirty-rect deferred; see
docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md.
EOF
)"
```

Replace `<n>` with the Step 5.3 measurement.

- [ ] **Step 5.9: Merge to `main`.**

Per AGENTS.md, land with a `--no-ff` merge so the feature stays grouped:

```bash
git checkout main
git merge --no-ff feat/v2b-async-qspi
git branch -d feat/v2b-async-qspi
```

Do **not** push unless the user explicitly asks.
