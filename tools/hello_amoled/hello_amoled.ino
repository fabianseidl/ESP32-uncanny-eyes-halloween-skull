/*
 * hello_amoled.ino
 *
 * Throwaway display smoke test for the Waveshare ESP32-S3-Touch-AMOLED-1.75
 * board (CO5300 466x466 QSPI AMOLED panel). This sketch is intentionally
 * self-contained and does NOT share any code with the main project. Its only
 * job is to prove that Arduino_GFX + the recorded pin map + the board's
 * power-on sequencing can bring up the panel.
 *
 * Arduino IDE board settings (install "esp32" by Espressif >= 3.3.5):
 *   Board:               "ESP32S3 Dev Module"
 *   USB CDC On Boot:     Enabled
 *   USB Mode:            Hardware CDC and JTAG
 *   Flash Mode:          QIO 80MHz
 *   Flash Size:          16MB (128Mb)
 *   PSRAM:               OPI PSRAM
 *   Partition Scheme:    Default 4MB with spiffs (or any default)
 *
 * Required library (install via Library Manager):
 *   "GFX Library for Arduino" by moononournation
 *
 * Expected behavior after upload:
 *   - Serial @115200 prints:
 *       hello_amoled: starting
 *       hello_amoled: display up
 *       hello_amoled: color 0     (panel fills RED)
 *       hello_amoled: color 1     (panel fills GREEN)
 *       hello_amoled: color 2     (panel fills BLUE)
 *       ... repeating every 2 seconds ...
 *   - Panel cycles solid red -> green -> blue forever with a 2 s dwell.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

// --- Pin map (from docs/hardware-notes.md, verified against Waveshare demo) ---
// QSPI bus to CO5300
static const int8_t LCD_CS_PIN    = 12;
static const int8_t LCD_SCLK_PIN  = 38;
static const int8_t LCD_SDIO0_PIN = 4;
static const int8_t LCD_SDIO1_PIN = 5;
static const int8_t LCD_SDIO2_PIN = 6;
static const int8_t LCD_SDIO3_PIN = 7;

// Panel reset is wired directly to GPIO39 (not through the TCA9554 expander).
static const int8_t LCD_RESET_PIN = 39;

// Shared I2C bus (not used for display bring-up, but other onboard devices
// sit on it so we still init the bus to match the Waveshare demo).
static const int8_t SDA_PIN = 15;
static const int8_t SCL_PIN = 14;

// Panel geometry. The CO5300 window offsets are REQUIRED on this board;
// col_offset1=6 avoids the 6-pixel horizontal garbage strip on the left.
static const int16_t LCD_WIDTH  = 466;
static const int16_t LCD_HEIGHT = 466;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS_PIN /* CS */, LCD_SCLK_PIN /* SCK */,
    LCD_SDIO0_PIN /* SDIO0 */, LCD_SDIO1_PIN /* SDIO1 */,
    LCD_SDIO2_PIN /* SDIO2 */, LCD_SDIO3_PIN /* SDIO3 */);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET_PIN /* RST */, 0 /* rotation */,
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */,
    6 /* col_offset1 */, 0 /* row_offset1 */,
    0 /* col_offset2 */, 0 /* row_offset2 */);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("hello_amoled: starting");

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!gfx->begin()) {
    Serial.println("hello_amoled: gfx->begin() FAILED");
    while (true) {
      delay(1000);
    }
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(200);

  Serial.println("hello_amoled: display up");
}

void loop() {
  // RGB565 fallbacks in case the installed library version uses different
  // constant names: RED=0xF800, GREEN=0x07E0, BLUE=0x001F.
  static const uint16_t colors[3] = {
      RGB565_RED,
      RGB565_GREEN,
      RGB565_BLUE,
  };

  for (uint8_t i = 0; i < 3; ++i) {
    gfx->fillScreen(colors[i]);
    Serial.print("hello_amoled: color ");
    Serial.println(i);
    delay(2000);
  }
}
