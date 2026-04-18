// display_async.cpp -- async QSPI pixel pusher. See display_async.h and
// docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md.
//
// Transaction-framing values (cmd=0x32, addr=0x003C00, QIO flags) are
// transcribed from Arduino_ESP32QSPI.cpp's writePixels() at installed
// library source ~/Documents/Arduino/libraries/GFX_Library_for_Arduino/
// src/databus/Arduino_ESP32QSPI.cpp:322-367. If that library file
// changes upstream, re-verify those values before upgrading.

#include "display_async.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <string.h>

// config.h drags in data/default_large.h which uses PROGMEM, so it must
// come AFTER <Arduino.h>. (Task 1 doesn't actually need config.h symbols
// yet, but Task 2 will pull QSPI_ASYNC_CHUNK_PX from here.)
#include "config.h"

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

// Task 1 stub fallback. Task 2 moves the real define into config.h;
// when that happens this #ifndef becomes a no-op.
#ifndef QSPI_ASYNC_CHUNK_PX
#define QSPI_ASYNC_CHUNK_PX 1024
#endif

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
