// display.ino -- Arduino_GFX wrapper for the Waveshare ESP32-S3-Touch-AMOLED-1.75
// (CO5300, QSPI, 466x466). All Arduino_GFX knowledge lives in this file.
//
// Pin values and constructor args come straight from Waveshare's official
// pin_config.h / 01_Hello_world.ino. See docs/hardware-notes.md for sources.

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// Pin map -- authoritative copy of Waveshare's pin_config.h.
#define PIN_QSPI_CS    12
#define PIN_QSPI_SCLK  38
#define PIN_QSPI_D0    4
#define PIN_QSPI_D1    5
#define PIN_QSPI_D2    6
#define PIN_QSPI_D3    7
#define PIN_LCD_RESET  39
#define PIN_I2C_SDA    15
#define PIN_I2C_SCL    14

// col_offset1=6 is required on this panel; omitting it leaves a 6px
// horizontal garbage strip on the left edge.
#define CO5300_COL_OFFSET1 6
#define CO5300_ROW_OFFSET1 0
#define CO5300_COL_OFFSET2 0
#define CO5300_ROW_OFFSET2 0

static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

void display_begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  s_bus = new Arduino_ESP32QSPI(
      PIN_QSPI_CS, PIN_QSPI_SCLK,
      PIN_QSPI_D0, PIN_QSPI_D1, PIN_QSPI_D2, PIN_QSPI_D3);

  s_gfx = new Arduino_CO5300(
      s_bus, PIN_LCD_RESET, 0 /* rotation */,
      PANEL_WIDTH, PANEL_HEIGHT,
      CO5300_COL_OFFSET1, CO5300_ROW_OFFSET1,
      CO5300_COL_OFFSET2, CO5300_ROW_OFFSET2);

  if (!s_gfx->begin()) {
    Serial.println("display_begin: gfx->begin() FAILED");
    while (true) { delay(1000); }
  }
}

void display_setBrightness(uint8_t value) {
  if (s_gfx) s_gfx->setBrightness(value);
}

void display_fillScreen(uint16_t color) {
  if (s_gfx) s_gfx->fillScreen(color);
}

void display_startWrite() {
  if (s_gfx) s_gfx->startWrite();
}

void display_endWrite() {
  if (s_gfx) s_gfx->endWrite();
}

void display_setAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (s_gfx) s_gfx->setAddrWindow(x, y, w, h);
}

// Push `len` RGB565 pixels. The renderer already byte-swaps each pixel
// into big-endian form before calling, so writePixels goes out as-is.
void display_writePixels(uint16_t *data, uint32_t len) {
  if (s_gfx) s_gfx->writePixels(data, len);
}
