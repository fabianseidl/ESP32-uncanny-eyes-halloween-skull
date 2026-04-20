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
