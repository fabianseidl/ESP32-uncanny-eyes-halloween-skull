# v2b — Async QSPI: pipelined DMA via a second `spi_device_handle_t`

**Status:** approved, ready for implementation
**Date:** 2026-04-19
**Follows:** [`2026-04-18-v2a-row-expand-design.md`](2026-04-18-v2a-row-expand-design.md) (full-panel 466×466 render at 10 fps measured, QSPI-bound).
**Scope:** v2b only — lift the v2a QSPI bottleneck by pipelining pixel DMA with CPU byte-swap / compute, entirely in app-space code. No dirty-rect, no renderer math changes, no library patch.

## Summary

v2a left us at a measured 10 fps on the Waveshare CO5300 AMOLED. Bus math from the v2a spec: 466² × 2 B × 10 fps ≈ 35 Mbps effective against a ~160 Mbps QSPI bus — wire is ~22 % utilized, so the limit is **blocking** (`spi_device_polling_start` immediately followed by `spi_device_polling_end` per 1024-px chunk in `Arduino_ESP32QSPI::writePixels`) plus per-chunk driver/command framing overhead, not raw bandwidth.

v2b adds a second `spi_device_handle_t` on the same `SPI2_HOST`, owned entirely by our repo. That handle does pixel-data bulk transfer with **queued, pipelined DMA**: byte-swap chunk N into one DMA-capable buffer, queue it via `spi_device_queue_trans()` (non-blocking), swap chunk N+1 into the other buffer while the first transfer is still in flight, wait for the first to finish via `spi_device_get_trans_result()`, queue the next. Compute and DMA overlap instead of serializing.

The `Arduino_GFX` library (`Arduino_CO5300` + `Arduino_ESP32QSPI`) remains **unmodified**. It keeps handling cold-path work: panel init, brightness, `setAddrWindow`, command framing, `fillScreen`. The hot-path pixel stream goes exclusively through our new `display_async.cpp` module.

The only library-interaction change is a one-line constructor flag: `Arduino_ESP32QSPI(..., is_shared_interface=true)`. Default `false` causes the library to hold the bus forever after `begin()`; `true` makes it acquire/release per `beginWrite()`/`endWrite()`, freeing the bus for our handle between phases.

## Goals

1. Sustained FPS ≥ 20 on the target board — target 30, measurement-gated follow-ups below 30.
2. Pixel output visually indistinguishable from v2a (same gaze motion, same blink, same iris scaling, same colors, same eyelid tracking).
3. No modification to any file under `~/Documents/Arduino/libraries/GFX_Library_for_Arduino/`. Library upgrades via `arduino-cli lib install` must not regress or break this work.
4. Renderer math untouched — `drawEyeRow()`, `expandRow()`, both Bresenham accumulators remain bit-for-bit identical to v2a. Only `emitRow()` / `emitRowFlushTail()` / `drawEye()`'s outer structure change.
5. All v2a invariants preserved: source/render-space boundary (`drawEyeRow` never references `RENDER_*`; `expandRow` never references pixel-decision logic); asset-size-agnostic renderer; left/right-eye mirror still works.
6. Memory budget: new static allocation ≤ 5 KB beyond v2a.

## Non-goals for v2b

- Dirty-rect / per-pixel change detection. Deferred to a possible follow-up spec if measurement lands in 20–29 fps.
- Byte-swap folded into `drawEyeRow()` or `expandRow()`. Byte-swap stays in the async module's `queueChunk` path.
- QSPI frequency bump beyond 40 MHz.
- Chunk-size bump beyond 1024 px (orthogonal future optimization; neither required nor excluded).
- Native-466 asset, bilinear interpolation, runtime-switchable asset tables. (Carried over from v2a.)
- Two-board sync, touch, IMU, mic/speaker, RTC, TF card, battery, OTA, web/BLE config. (v2c+.)

## Target hardware

Unchanged from v1/v2a: Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI, 466×466, 40 MHz). No new chip usage.

## Architecture

### Two handles, one bus

Two `spi_device_handle_t` live on `SPI2_HOST` after `display_begin()` returns:

- **Handle L** (library-owned). Created by `Arduino_ESP32QSPI::begin()` inside `s_gfx->begin()`. Used by the library for every non-pixel operation: `setAddrWindow`, `writeCommand*`, `setBrightness`, `fillScreen`, all CO5300 init-sequence bytes. `queue_size = 1`.
- **Handle A** (ours). Created by `display_async_init()` after `s_gfx->begin()` via `spi_bus_add_device()`. Used exclusively for pixel-data bulk transfer. `queue_size = 3`.

Device configuration (`spi_device_interface_config_t`) matches the library's exactly apart from `queue_size`: same `command_bits=8`, `address_bits=24`, `mode`, `clock_speed_hz`, `flags=SPI_DEVICE_HALFDUPLEX`, and crucially `spics_io_num = -1` (manual CS via GPIO — identical to the library's scheme, no hardware CS conflict).

### Bus-share coordination

`Arduino_ESP32QSPI` is instantiated with `is_shared_interface = true`. Effects:

- `begin()` skips the unconditional `spi_device_acquire_bus(handle_L, portMAX_DELAY)` it normally issues; bus is free between calls.
- `beginWrite()` acquires via `spi_device_acquire_bus(handle_L, ...)`; `endWrite()` releases.
- Every library command path goes through `beginWrite`/`endWrite`, so the library holds the bus only for its own (short) operations.

Our handle-A path mirrors the same discipline: `display_pixelsBegin()` calls `spi_device_acquire_bus(handle_A, ...)`; `display_pixelsEnd()` calls `spi_device_release_bus(handle_A)`.

`drawEye()` becomes a **two-phase** function:

```c
void drawEye(...) {
  // COLD PHASE — library holds bus
  display_startWrite();           // spi_device_acquire_bus(handle_L)
  display_setAddrWindow(0,0,466,466);
  display_endWrite();             // spi_device_release_bus(handle_L)

  // HOT PHASE — we hold bus
  display_pixelsBegin();          // spi_device_acquire_bus(handle_A), CS low
  for (source row loop) {
    drawEyeRow(...); expandRow(...); emitRow(line_dst);
    // + vertical Bresenham extra emits, unchanged from v2a
  }
  emitRowFlushTail();
  display_pixelsEnd();            // drain DMA queue, CS high, spi_device_release_bus(handle_A)
}
```

**Invariant:** at any moment at most one of `{handle_L, handle_A}` holds the bus. Phases never nest. `pixelsBegin/End` and `startWrite/endWrite` are in disjoint code ranges.

### The async module (`display_async.cpp`)

**C API**

```c
void display_async_init();                                        // called from display_begin()
void display_pixelsBegin();                                       // acquire bus, CS low, reset state
void display_pixelsQueueChunk(const uint16_t* px, uint32_t len);  // byte-swap + queue; len ≤ QSPI_ASYNC_CHUNK_PX
void display_pixelsEnd();                                         // drain queue, CS high, release bus
```

**State (file-scope statics)**

```c
static spi_device_handle_t   s_handle;
static uint16_t*             s_dma_buf[2];     // 2 × 2 KB, MALLOC_CAP_DMA, 16-byte aligned
static spi_transaction_ext_t s_trans[2];
static uint8_t               s_buf_idx;        // 0 or 1 — next slot to fill
static bool                  s_first_chunk;    // needs cmd+addr framing
static bool                  s_inflight[2];    // DMA currently queued for this slot?
```

**Transaction format** — transcribed from `Arduino_ESP32QSPI.cpp` lines 332–344 (checked against freshly-installed library at spec time):

- First chunk after `pixelsBegin`: `flags = SPI_TRANS_MODE_QIO`, `cmd = 0x32`, `addr = 0x003C00`. This is the CO5300 quad-write framing that enters RAMWR mode.
- Continuation chunks: `flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY`, with `ext.command_bits = 0`, `ext.address_bits = 0`, `ext.dummy_bits = 0`. Pure data.

A comment in `display_async.cpp` cites the library source file + line so future readers can diff against upstream.

**`pixelsQueueChunk` state machine**

```text
slot = s_buf_idx

if s_inflight[slot]:                           // previous DMA into this buffer still ongoing
  spi_device_get_trans_result(s_handle, ..., portMAX_DELAY)
  s_inflight[slot] = false

swap_rgb565_be(s_dma_buf[slot], src, len)      // CPU byte-swap ~5 µs / 1024 px

configure s_trans[slot] for first-chunk or continuation per above
s_trans[slot].base.tx_buffer = s_dma_buf[slot]
s_trans[slot].base.length    = len * 16        // bits

spi_device_queue_trans(s_handle, &s_trans[slot].base, portMAX_DELAY)
s_inflight[slot] = true
s_first_chunk    = false

s_buf_idx ^= 1                                 // flip slot for next call
```

Up to 2 queued transactions at any time (one executing in the QSPI peripheral, one pending in the ESP-IDF driver queue, ready to chain back-to-back without CPU intervention). This is where the pipelining lives: while the hardware transfers slot N, `queueChunk` for N+1 can byte-swap and queue without waiting. CPU work (including the caller's next row compute) overlaps with DMA wire time. `queue_size = 3` gives a one-slot safety margin over our 2-queued ceiling — the driver's internal queue never back-pressures us in practice.

**`pixelsBegin`**

```text
spi_device_acquire_bus(s_handle, portMAX_DELAY)
CS_LOW                              // direct GPIO write via same trick the library uses
s_buf_idx     = 0
s_first_chunk = true
// s_inflight[] is already {false,false} from last pixelsEnd
```

**`pixelsEnd`**

```text
for slot in 0..1:
  if s_inflight[slot]:
    spi_device_get_trans_result(s_handle, ..., portMAX_DELAY)
    s_inflight[slot] = false
CS_HIGH
spi_device_release_bus(s_handle)
```

`CS_HIGH` always happens AFTER both slots drain — any transaction still transferring when CS deasserts would truncate the last row.

**Byte-swap** is a tight inline loop: `(p << 8) | (p >> 8)`. The compiler lifts this to the Xtensa byte-reverse idiom and typically auto-vectorizes. ~5 µs per 1024-px chunk on ESP32-S3 @ 240 MHz — negligible against the ~102 µs DMA wire time per chunk.

### Renderer changes

`emitRow()` and `emitRowFlushTail()` shift from the renderer's own `pbuffer[2][BUFFER_SIZE]` ping-pong + blocking `display_writePixels()` to a single accumulator + async queue:

```c
static uint16_t s_chunk_buf[QSPI_ASYNC_CHUNK_PX];   // 1024 px = 2 KB, plain RAM
static uint32_t s_chunk_fill = 0;

static void emitRow(const uint16_t* line_dst) {
  for (uint32_t i = 0; i < RENDER_WIDTH; i++) {
    s_chunk_buf[s_chunk_fill++] = line_dst[i];
    if (s_chunk_fill == QSPI_ASYNC_CHUNK_PX) {
      display_pixelsQueueChunk(s_chunk_buf, s_chunk_fill);
      s_chunk_fill = 0;
    }
  }
}

static void emitRowFlushTail() {
  if (s_chunk_fill) {
    display_pixelsQueueChunk(s_chunk_buf, s_chunk_fill);
    s_chunk_fill = 0;
  }
}
```

Changes from v2a:

- `pbuffer[2][1024]` ping-pong → single `s_chunk_buf[1024]`. The async layer holds its own DMA-capable ping-pong internally, so renderer-side ping-pong is redundant.
- `dmaBuf` toggle variable, `s_emitPixels` globalized state — all removed.
- `yield()` in the chunk-flush path removed. With `polling_start/end` replaced by queued DMA, `spi_device_get_trans_result` yields via semaphore. Manual `yield()` in a tight hot loop is no longer needed to keep FreeRTOS fed.
- Renderer no longer references `display_writePixels()` or `pbuffer[]` at all.

`drawEye()` gains the cold/hot split shown in the architecture section. `drawEyeRow()`, `expandRow()`, vertical Bresenham math — **all unchanged**.

### Initialization sequence (in `display_begin()`)

```text
1. Wire.begin(SDA=15, SCL=14)
2. s_bus = new Arduino_ESP32QSPI(CS=12, SCK=38, D0=4, D1=5, D2=6, D3=7,
                                 /*is_shared_interface=*/true)       // ← was false
3. s_gfx = new Arduino_CO5300(s_bus, RESET=39, rotation=0,
                              PANEL_W=466, PANEL_H=466, col/row offsets)
4. s_gfx->begin()
     ├─ internally: spi_bus_initialize(SPI2_HOST, ...)
     ├─ internally: spi_bus_add_device(SPI2_HOST, devcfg_L, &handle_L)
     └─ CO5300 command sequence (reset, sleep-out, color-mode, etc.)
5. display_async_init()
     ├─ spi_bus_add_device(SPI2_HOST, devcfg_A, &handle_A)   // queue_size=3
     ├─ s_dma_buf[0] = heap_caps_aligned_alloc(16, 2*QSPI_ASYNC_CHUNK_PX, MALLOC_CAP_DMA)
     ├─ s_dma_buf[1] = heap_caps_aligned_alloc(16, 2*QSPI_ASYNC_CHUNK_PX, MALLOC_CAP_DMA)
     └─ Serial.println("qspi_async: init ok")
```

`spi_bus_initialize` is called exactly once per boot (by the library). Our code only adds a second device to the already-initialized bus.

### Memory layout

| Buffer | Size | Purpose | Delta vs v2a |
|---|---|---|---|
| `line_src[SCREEN_WIDTH]` | 480 B | Source-row buffer, unchanged from v2a | 0 |
| `line_dst[RENDER_WIDTH]` | 932 B | Expanded render-row buffer, unchanged from v2a | 0 |
| `s_chunk_buf[1024]` | 2 KB | Renderer accumulator (replaces `pbuffer[2][1024]`) | -2 KB |
| `s_dma_buf[0..1]` | 4 KB | Async module DMA ping-pong | +4 KB |
| `s_trans[2]` + driver queue | ~320 B | Transaction state | +320 B |

Net delta: **~+2 KB static RAM**. Well within budget.

## Configuration model (`config.h`)

Add one constant; no other changes.

```c
// Chunk size for async QSPI pixel pushes. Must equal ESP32QSPI_MAX_PIXELS_AT_ONCE
// (1024 default) since our transaction format (cmd=0x32 addr=0x003C00 on first
// chunk, continuation otherwise) assumes the library's per-chunk framing.
#define QSPI_ASYNC_CHUNK_PX 1024
```

No changes to `SCREEN_*`, `RENDER_*`, `PANEL_*`, `EYE_SIDE`, `DISPLAY_BRIGHTNESS`, `IRIS_MIN`/`MAX`, `TRACKING`, `AUTOBLINK`, `IRIS_SMOOTH`.

## File changes

| File | Change |
|---|---|
| `config.h` | +1 constant (`QSPI_ASYNC_CHUNK_PX`). |
| `ESP32-uncanny-eyes-halloween-skull.ino` | Remove `pbuffer[2][]`, `dmaBuf`, `#define BUFFER_SIZE`, `#define BUFFERS`. |
| `eye_functions.ino` | Rewrite `emitRow()` and `emitRowFlushTail()` per the renderer-changes section. Split `drawEye()` cold/hot. Remove `yield()`. Remove file-scope `s_emitPixels`. `drawEyeRow()`, `expandRow()`, vertical Bresenham — unchanged. `frame()`, `updateEye()`, `split()`, `initEyes()` — unchanged. |
| `display.ino` | Constructor arg flip to `is_shared_interface=true`. Call `display_async_init()` after `s_gfx->begin()`. Remove `display_writePixels()`. |
| `display_async.cpp` | **New.** ~120 LOC: device init, DMA buffer alloc, `pixelsBegin/QueueChunk/End`. |
| `README.md` | Targeted edits: note async-QSPI implementation, refresh FPS number after measurement, mark dirty-rect as deferred. |
| `docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md` | New — this document. |
| No library files. | `~/Documents/Arduino/libraries/GFX_Library_for_Arduino/` is not edited. |

## Key decisions

1. **Second `spi_device_handle_t` in app space, not a library patch.** ESP-IDF natively supports multiple devices per bus. The library already uses `spics_io_num = -1` (manual CS) — our handle uses the same scheme, so CS is software-owned by whichever phase holds the bus. Library upgrades don't touch our code path.
2. **`is_shared_interface=true` on the library.** One-line flip unblocks the shared-bus model. Without this, the library calls `spi_device_acquire_bus(handle_L, portMAX_DELAY)` in `begin()` and never releases, which would starve our handle.
3. **Keep `Arduino_CO5300` for cold path, not just init.** `setAddrWindow` and brightness/fill remain library calls. Reason: those paths are well-tested, infrequent (once per frame for `setAddrWindow`, once at boot for brightness), and rewriting them ourselves would re-implement CO5300 command semantics we don't need to own.
4. **Pixel-transaction format transcribed exactly from the library.** `cmd=0x32 addr=0x003C00` on first chunk, continuation framing afterward. Duplicates two places the CO5300 pixel-stream protocol exists in the codebase, but the alternative (reaching into library privates) is architecturally worse. A comment cites the library source line so future review can diff.
5. **Two-buffer ping-pong, up to 2 queued transactions, `queue_size = 3`.** One slot executing in hardware, one pending in the driver's queue. Next `queueChunk` call waits on the older slot to complete, then queues a new one. This gives real CPU-DMA overlap (byte-swap + next-row compute run during DMA) while keeping the state machine small — two bool flags and one index. `queue_size = 3` is one-slot headroom over our 2-queued ceiling; cost is ~120 B in the driver's queue and zero runtime.
6. **Byte-swap inside `queueChunk`, not folded into renderer.** Keeps the renderer code path bit-for-bit v2a. Folding byte-swap into `drawEyeRow()` or `expandRow()` is a follow-up optimization that saves one pass over `line_dst` (~2 µs per row) but couples render-space code to output endianness. Not worth the coupling for ~1 ms/frame.
7. **Drop renderer-side ping-pong.** The async layer owns DMA ping-pong internally; renderer keeps one 2 KB accumulator. Net -2 KB renderer buffer, +4 KB async DMA buffer. Simpler renderer state; single source of "what pixel-push state are we in".
8. **Drop `yield()` from hot-loop flush.** Manual `yield()` existed in v2a because `polling_start/end` busy-spun for ~100 µs/chunk. Queued DMA waits on a semaphore (`spi_device_get_trans_result`), which yields to FreeRTOS natively. Manual yield in the hot loop only adds jitter.
9. **FPS floor at 20 with 30 as target, not floor at 30.** Realistic pipelining math (Round-5 analysis: ~22 ms DMA time recoverable from 100 ms serialized frame → ~13 FPS pure gain + unknown driver-overhead reduction) suggests first-measurement 15–25 FPS range. Measurement-gated follow-ups below 30 (dirty-rect spec for 20–29, profiling spec for <20). Separates "scaling correctness" (v2a) from "pipelining correctness" (v2b) from "semantic dirty-rect" (v2c-optional) as independent concerns.

## Known risks / things to verify during v2b bring-up

1. **Bus-share flag not propagating.** Symptom: `spi_device_acquire_bus(handle_A, ...)` hangs, watchdog reset ~5 s after first frame attempt. *Verification:* serial log "qspi_async: bus acquired" inside first `pixelsBegin`. If it never prints, the library is still holding the bus from `begin()` — constructor flag isn't `true`.
2. **CO5300 pixel framing mismatch.** Symptom: garbage, shifted, or scrolling image; panel may stay blank. *Verification:* transcribe `cmd/addr/flags` values from the current installed library's `Arduino_ESP32QSPI.cpp:322–366` exactly, with a code comment citing the line range. Compare-on-sight at first flash.
3. **DMA transaction undrained at frame end.** Symptom: intermittent tearing, last rows of frame missing. *Verification:* `pixelsEnd` loops `get_trans_result` until both `s_inflight[0]` and `s_inflight[1]` are false, BEFORE `CS_HIGH`.
4. **Byte-swap direction wrong.** Symptom: colors psychedelic (red↔blue swapped) but shape coherent. *Verification:* boot issues a known-color `display_fillScreen(0xF800)` (red in v1 convention) before the eye appears.
5. **DMA cache coherency.** Symptom: intermittent pixel snow. *Verification:* buffers allocated with `heap_caps_aligned_alloc(16, size, MALLOC_CAP_DMA)` → placed in internal SRAM, no cache line. Runtime assert on the alloc returning non-NULL.
6. **CS deasserted mid-transfer.** Symptom: last 1–2 rows of frame corrupted. *Verification:* `pixelsEnd` drains all slots before `CS_HIGH`. Runtime debug assert: `s_inflight[0] == false && s_inflight[1] == false` at the moment of `CS_HIGH`.
7. **`queue_size` too small under pressure.** Symptom: `spi_device_queue_trans` blocks unexpectedly, FPS no better than v2a. *Verification:* `queue_size = 3` vs our 2-queued ceiling gives one slot of headroom. If observed, bump to 4.
8. **First-chunk framing after every `pixelsBegin`.** Symptom: panel hang or garbage only at frame boundaries after the first frame. *Verification:* `s_first_chunk = true` is set in every `pixelsBegin`, not just at init.
9. **FPS < 20 after landing.** Not a defect; triggers a follow-up profiling spec per the measurement gate in Success Criteria. Timing probes (`esp_timer_get_time()`) around `pixelsBegin…pixelsEnd`, `drawEyeRow`, and `get_trans_result` inform the profiling spec.
10. **Shared-bus coordination violation.** Symptom: intermittent visible glitches only when a library call (e.g. `display_setBrightness` issued off-frame) lands between pixel chunks. *Verification:* all library writes live outside `pixelsBegin…pixelsEnd` pairs by code structure. `drawEye()` is the only hot-path function; other library calls happen in `setup()` or never in the render loop.

## Success criteria (v2b "done")

All of the following on the target board:

- Sketch builds from `arduino-cli compile` with zero warnings beyond v2a baseline.
- Boot is clean — no crash, no brownout, no `gfx->begin() FAILED`, no `spi_bus_add_device` error. Serial shows `qspi_async: init ok` after `uncanny-eyes: display_begin()`.
- Within ~2 s the eye appears filling the full 466×466 AMOLED. Image is visually indistinguishable from v2a: same gaze motion, same blink timing, same iris scaling, same colors, same eyelid tracking, same left/right mirror.
- `EYE_SIDE = LEFT` and `EYE_SIDE = RIGHT` builds both render correctly.
- Serial `FPS=<n>` reports ≥ 20 sustained. ≥ 30 clears the target. 20–29 is acceptable and opens a dirty-rect follow-up spec. < 20 is not acceptable and opens a profiling follow-up spec.
- Library integrity: `Arduino_ESP32QSPI.cpp` / `Arduino_ESP32QSPI.h` at `~/Documents/Arduino/libraries/GFX_Library_for_Arduino/src/databus/` are byte-identical to a freshly-installed copy from Arduino Library Manager (sha256 check acceptable). Only `display.ino`'s constructor arg differs from v2a in library-touching code.
- Renderer integrity: `drawEyeRow()`, `expandRow()`, the horizontal/vertical Bresenham math, `frame()`, `updateEye()`, `split()`, `initEyes()` diff against v2a shows no logic change — only `emitRow()`, `emitRowFlushTail()`, `drawEye()`'s outer structure, and removed globals differ.
- Memory sanity: `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)` at end of `setup()` is ≥ 50 KB.
- Code review: no surviving caller of `display_writePixels()`; function declaration removed from `display.ino`.

## Out of scope — explicit non-work list

- Any dirty-rect or change-detection logic.
- Renderer byte-swap folding.
- QSPI clock bump beyond 40 MHz.
- `ESP32QSPI_MAX_PIXELS_AT_ONCE` bump beyond 1024.
- Native-466 asset regeneration. Bilinear interpolation.
- Runtime-switchable asset tables.
- Two-board sync, touch, IMU, mic/speaker, RTC, TF card.
- Battery / low-power, OTA, web/BLE config.

## Future work (sketch only, not committed)

- **v2c-dirty-rect (conditional on v2b measurement).** Semantic classification per frame: `gazeMoved` → full panel; `irisChanged` alone → iris bounding box; `lidChanged` alone → eyelid strips. Up to 3 address windows per frame. Per-rect source-row range + render-column range to `drawEye`'s outer loop. Renderer becomes a clip-aware variant of the v2a full-panel path. Expected additional ~1.5–2× on held-gaze frames. Spec written only if v2b lands in 20–29 fps.
- **v2c-profiling (conditional on v2b measurement).** If v2b < 20 fps, instrument `drawEye` with `esp_timer_get_time` probes around cold phase, hot phase, per-row compute, per-chunk queue-to-result latency. Publish per-phase breakdown. Design against measured attribution. Spec written only if v2b < 20 fps.
- **v2d-chunk-size bump.** `#define ESP32QSPI_MAX_PIXELS_AT_ONCE 4096` via build flag. Orthogonal to v2b; stackable. Reduces per-chunk driver overhead ~4×. +12 KB DMA heap. Easy A/B measurement.
- **v2d-byte-swap folding.** Fold byte-swap into `drawEyeRow()` or `expandRow()`. Saves one pass over `line_dst` per row (~2 µs). Couples render-space code to output endianness. Only worth doing if bounded by byte-swap time specifically.
- **v2d-upstream PR.** Propose async `writePixels` to `moononournation/Arduino_GFX`. If accepted, eventually replaces `display_async.cpp`. Orthogonal good-citizenship work.
- **v2d+ (unchanged from v1/v2a specs):** second eye with ESP-NOW sync, touch-to-blink, IMU head-tracking, mic-triggered reactions. Each its own spec.
