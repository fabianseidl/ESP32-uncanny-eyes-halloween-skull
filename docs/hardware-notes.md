# Waveshare ESP32-S3-Touch-AMOLED-1.75 — hardware notes

Source(s):
- Product page: https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm
- Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- Official demo repo: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
- `pin_config.h` (authoritative pin map): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h
- `01_Hello_world.ino` (minimal display bring-up): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/01_Hello_world/01_Hello_world.ino
- `06_LVGL_Widgets.ino` (touch + IMU + display): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/06_LVGL_Widgets/06_LVGL_Widgets.ino
- `05_LVGL_AXP2101_ADC_Data.ino` (PMU demo): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/05_LVGL_AXP2101_ADC_Data/05_LVGL_AXP2101_ADC_Data.ino
- Full-board schematic PDF: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf
- Arduino_GFX `Arduino_CO5300.h` (for constructor signature + default init): https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_CO5300.h
- Arduino_GFX `Arduino_ESP32QSPI.h` (for default QSPI frequency): https://github.com/moononournation/Arduino_GFX/blob/master/src/databus/Arduino_ESP32QSPI.h
- Waveshare `ESP32_IO_Expander` TCA95xx_8bit driver (for address encoding): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/libraries/ESP32_IO_Expander/src/chip/TCA95xx_8bit.h

Captured: 2026-04-18

## CO5300 AMOLED (QSPI)

| Signal    | ESP32-S3 GPIO |
| --------- | ------------- |
| QSPI CS   | GPIO12        |
| QSPI SCLK | GPIO38        |
| QSPI D0   | GPIO4         |
| QSPI D1   | GPIO5         |
| QSPI D2   | GPIO6         |
| QSPI D3   | GPIO7         |
| TE        | GPIO13 (wired on the display FPC, but the Waveshare Arduino demo does NOT use it — it is not declared in `pin_config.h`, and `Arduino_CO5300` operates by polling without a TE pin. Pass `-1` when porting unless you add explicit TE handling.) |
| RESET     | GPIO39 (direct ESP32-S3 GPIO; see "Display reset wiring" below) |

QSPI clock used by demo: **40 MHz**.
(Rationale: the demo calls `gfx->begin()` with no speed argument, which falls back to `Arduino_ESP32QSPI`'s compile-time default `ESP32QSPI_FREQUENCY = 40000000`. See `Arduino_ESP32QSPI.h`.)

Display reset wiring: **direct GPIO39**, NOT through the TCA9554 expander.
- `pin_config.h` declares `#define LCD_RESET 39`.
- The demo instantiates the display as `Arduino_CO5300(bus, LCD_RESET /* 39 */, ...)` and relies on Arduino_GFX to toggle that pin during `tftInit()`.
- The schematic's J3 display-FPC connector routes `LCD_RESET` directly to the ESP32-S3 GPIO39 net — not to an EXIOx net of the TCA9554.
- The `01_Hello_world` sketch does not initialize the TCA9554 at all and the display still comes up, confirming the expander is not on the display-reset path.

## I²C bus

| Signal | ESP32-S3 GPIO |
| ------ | ------------- |
| SDA    | GPIO15        |
| SCL    | GPIO14        |

Single shared I²C bus. `pin_config.h` declares `#define IIC_SDA 15` / `#define IIC_SCL 14`. Every demo calls `Wire.begin(IIC_SDA, IIC_SCL)` and then talks to CST9217 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMU, TCA9554 expander, and ES8311 codec on that bus.

Known 7-bit addresses on the bus (for reference):

| Device   | Address | Evidence |
| -------- | ------- | -------- |
| CST9217 touch    | 0x5A | `touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)` in `06_LVGL_Widgets.ino` |
| QMI8658 IMU      | 0x6B | "0X6B" label next to QMI8658 block in schematic; SDO/SA0 tied via that strap |
| PCF85063 RTC     | 0x51 | PCF85063 datasheet fixed address (not user-configurable); not explicitly written as hex in Waveshare code |
| AXP2101 PMU      | 0x34 | `power.begin(Wire, AXP2101_SLAVE_ADDRESS, ...)` in `05_LVGL_AXP2101_ADC_Data.ino`; `AXP2101_SLAVE_ADDRESS == 0x34` in XPowersLib |
| TCA9554PWR       | 0x20 | A0/A1/A2 pins of U5 tied to GND in schematic → 7-bit base 0x20 per `TCA95xx_8bit.h` address encoding |
| ES8311 codec     | 0x18 | `es8311_create(0, ES8311_ADDRRES_0)` in `08_ES8311.ino`; `ES8311_ADDRRES_0 == 0x18` in esp_codec ES8311 driver |

## TCA9554 GPIO expander

- I²C address: **0x20** (A0 = A1 = A2 = GND on the schematic).
- Pin used as CO5300 RESX: **NONE** — the CO5300 reset is wired directly to ESP32-S3 GPIO39 on this board, not through the expander. Do not try to drive reset via the expander.
- Other display/touch/audio pins this design touches via the expander: **NONE confirmed**. None of the eight Arduino example sketches in `examples/Arduino-v3.3.5/examples/` instantiate `ESP_IOExpander_TCA95xx_8bit` or otherwise touch the expander. The schematic shows P0–P3 (EXIO0–EXIO3) broken out to test points TP4–TP7, and P4–P7 (EXIO4–EXIO7) routed into the LC76G / GPS / misc support circuitry — none of which are on the critical path for display bring-up.
- The `ESP32_IO_Expander` library is listed in the wiki's required-libraries table, but on this base 1.75 board it is effectively unused for display/touch. (It may be used on the related `ESP32-S3-Touch-AMOLED-1.75C` variant — confirm on that board separately if targeting it.)

## AXP2101 PMU

- I²C address: **0x34** (`AXP2101_SLAVE_ADDRESS`).
- Register writes performed by the Waveshare demo **before** display init (`gfx->begin()`):
  - **None required.** The `01_Hello_world` sketch brings the CO5300 up successfully with no prior AXP2101 interaction at all — it never constructs an `XPowersPMU` object and never writes to the PMU. The 3.3V rail that feeds the display panel is already on via the AXP2101's power-on / NVM default configuration.
  - In `05_LVGL_AXP2101_ADC_Data.ino`, the only PMU calls made before `gfx->begin()` are:
    - `power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)` — chip probe/detect, no rail changes.
    - `power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ)`, `power.setChargeTargetVoltage(3)`, `power.clearIrqStatus()`, `power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ)` — charger/IRQ configuration, no rail changes.
    - `adcOn()` — only enables the PMU's internal ADC readout channels (temperature, Vbat, Vbus, Vsys, battery detection); does not enable any DLDO/ALDO/BLDO/DCDC rail.
  - No `setDLDOx`, `setALDOx`, `setBLDOx`, `setDCDCx`, `enableDLDOx`, `enableALDOx`, `enableBLDOx`, or `enableDCDCx` calls exist in any of the eight Arduino example sketches. **Grep for `ALDO|DLDO|BLDO|DCDC|setALDO|setDLDO|setBLDO|enableDLDO|enableALDO|enableBLDO|writeRegister` across all eight example `.ino` files returned zero hits.**
  - Conclusion: for a plain port, `gfx->begin()` may be called without any prior AXP2101 register writes. Only instantiate `XPowersPMU` if you need charger/ADC telemetry or a proper low-power shutdown.

## CO5300 init quirks

- `col_offset1` / `row_offset1` values passed to `setAddrWindow()` (via the `Arduino_CO5300` constructor): **`col_offset1 = 6`, `row_offset1 = 0`**.
- `col_offset2` / `row_offset2` values: **`col_offset2 = 0`, `row_offset2 = 0`**.
- Source: every Waveshare example constructs the display as
  `new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH /* 466 */, LCD_HEIGHT /* 466 */, 6, 0, 0, 0);`
  The constructor signature in `Arduino_CO5300.h` is
  `Arduino_CO5300(Arduino_DataBus *bus, int8_t rst, uint8_t r, int16_t w, int16_t h, uint8_t col_offset1, uint8_t row_offset1, uint8_t col_offset2, uint8_t row_offset2)`
  so the trailing `6, 0, 0, 0` maps to col_offset1=6, row_offset1=0, col_offset2=0, row_offset2=0.
  The `col_offset1 = 6` exists because the CO5300's internal framebuffer origin is offset by 6 columns from the 466-pixel visible area on this particular panel/glass cut. Omitting it causes a 6-pixel horizontal shift (typically manifests as a strip of garbage on one edge).
- Any custom init commands beyond what Arduino_GFX's `Arduino_CO5300` class does by default: **none**. The demos do not send any extra `writeCommand()` / `writeData()` sequence before or after `gfx->begin()`. The built-in `co5300_init_operations[]` table in `Arduino_CO5300.h` is all that is needed.

## Source snippets

### `libraries/Mylibrary/pin_config.h` — authoritative pin map

```c
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h
#pragma once

#define XPOWERS_CHIP_AXP2101

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

// TOUCH
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

// ES8311
#define I2S_MCK_IO 16
#define I2S_BCK_IO 9
#define I2S_DI_IO 10
#define I2S_WS_IO 45
#define I2S_DO_IO 8

#define MCLKPIN             42
#define BCLKPIN              9
#define WSPIN               45
#define DOPIN               10
#define DIPIN                8
#define PA                  46

// SD
const int SDMMC_CLK = 2;
const int SDMMC_CMD = 1;
const int SDMMC_DATA = 3;
const int SDMMC_CS = 41;
```

### `examples/01_Hello_world/01_Hello_world.ino` — minimal display bring-up

```cpp
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/01_Hello_world/01_Hello_world.ino
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
  LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH /* width */, LCD_HEIGHT /* height */, 6, 0, 0, 0);

void setup(void) {
  Serial.begin(115200);
  Wire.begin(IIC_SDA, IIC_SCL);

  if (!gfx->begin()) {          // no speed arg -> Arduino_ESP32QSPI default: 40 MHz
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(128);
  // ... (remainder: prints "Hello World!")
}
```

Note that this sketch does **not** touch AXP2101, TCA9554, or any other I²C device before `gfx->begin()`. The only prior I²C action is `Wire.begin(IIC_SDA, IIC_SCL)` to initialize the bus itself.

### `Arduino_CO5300.h` — constructor signature (to verify offset argument order)

```cpp
// https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_CO5300.h
class Arduino_CO5300 : public Arduino_OLED
{
public:
  Arduino_CO5300(
      Arduino_DataBus *bus, int8_t rst = GFX_NOT_DEFINED, uint8_t r = 0,
      int16_t w = CO5300_TFTWIDTH, int16_t h = CO5300_TFTHEIGHT,
      uint8_t col_offset1 = 0, uint8_t row_offset1 = 0,
      uint8_t col_offset2 = 0, uint8_t row_offset2 = 0);
  // ...
};
```

So in `new Arduino_CO5300(bus, LCD_RESET, 0, 466, 466, 6, 0, 0, 0)` the trailing `6, 0, 0, 0` is unambiguously `col_offset1=6, row_offset1=0, col_offset2=0, row_offset2=0`.

### `Arduino_ESP32QSPI.h` — default QSPI frequency

```cpp
// https://github.com/moononournation/Arduino_GFX/blob/master/src/databus/Arduino_ESP32QSPI.h
#ifndef ESP32QSPI_FREQUENCY
#define ESP32QSPI_FREQUENCY 40000000
#endif
```

`gfx->begin()` with no argument uses this value → **40 MHz** on the Waveshare demo.

### `examples/05_LVGL_AXP2101_ADC_Data.ino` — every PMU-related line run before `gfx->begin()`

```cpp
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/05_LVGL_AXP2101_ADC_Data/05_LVGL_AXP2101_ADC_Data.ino
Wire.begin(IIC_SDA, IIC_SCL);

bool result = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
if (result == false) { /* fatal */ }

power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
power.setChargeTargetVoltage(3);
power.clearIrqStatus();
power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

adcOn();                 // enables PMU-internal ADC channels only, no rail enables

gfx->begin();            // <-- no DLDO/ALDO/BLDO/DCDC writes happened before this
```

### `examples/06_LVGL_Widgets.ino` — confirms reset pins are direct GPIOs

```cpp
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/examples/Arduino-v3.3.5/examples/06_LVGL_Widgets/06_LVGL_Widgets.ino
Wire.begin(IIC_SDA, IIC_SCL);

digitalWrite(TP_RESET, LOW);   // TP_RESET == GPIO40, driven directly
delay(30);
digitalWrite(TP_RESET, HIGH);
delay(50);

touch.setPins(TP_RESET, TP_INT);
bool result = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);   // CST9217 @ 0x5A
// ...
qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL); // QMI8658 on same bus

gfx->begin();   // no expander calls anywhere in this file
```

`TP_RESET` (GPIO40) and by extension `LCD_RESET` (GPIO39) are toggled directly with `digitalWrite()` on ESP32-S3 GPIOs — no TCA9554 involvement.

### Schematic cross-checks

From `Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` (`pdftotext`-extracted netlist, U5 = TCA9554PWR block):

- `LCD_CS`, `QSPI_SIO0`, `QSPI_SI1`, `QSPI_SI2`, `QSPI_SI3`, `QSPI_SCL`, `LCD_RESET`, `LCD_TE` on the J3 display FPC connector map to ESP32-S3 nets `GPIO12, GPIO4, GPIO5, GPIO6, GPIO7, GPIO38, GPIO39, GPIO13` respectively (agrees with `pin_config.h` except for `LCD_TE`, which the header omits because the demo does not use it).
- U5 pin-1 (A0), pin-2 (A1), pin-3 (A2) all tie to GND (with optional 0 Ω depop resistor footprints), giving the TCA9554 a 7-bit address of **0x20**.
- U5 pins 4–7, 9–12 (P0–P3, P4–P7) route to nets `EXIO0..EXIO3` (test points TP4–TP7) and `EXIO4..EXIO7` (miscellaneous support — GPS reset, etc., not display/touch).
- `PA_CTRL` (audio amp enable) goes from ESP32-S3 **GPIO46** through a 0 Ω series resistor straight to the amp — NOT through the expander (`pin_config.h` agrees: `#define PA 46`).

This confirms the three key facts for the port:
1. Display reset is on ESP32-S3 GPIO39 (not the expander).
2. Touch reset is on ESP32-S3 GPIO40 (not the expander).
3. Audio PA enable is on ESP32-S3 GPIO46 (not the expander).

For a display-and-touch-only port (which is all the Uncanny Eyes project needs), the TCA9554 can be ignored entirely.
