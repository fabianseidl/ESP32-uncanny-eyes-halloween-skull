// Uncanny Eyes -- Waveshare ESP32-S3-Touch-AMOLED-1.75 port (v2b async QSPI).
//
// Renders one eye (runtime side from STA MAC; see config.h EYE_SIDE_MAC_*).
// AMOLED, NN-stretched from the 240-baked asset via a row expander. Pixel
// stream uses display_async (second SPI device + DMA). See
// docs/superpowers/specs/2026-04-19-v2b-async-qspi-design.md.

#include "config.h"
#include "eyes.h"
#include "eye_gallery.h"
#include "eye_side.h"
#include "eye_sync.h"

// Row-expand line buffers (see docs/superpowers/specs/2026-04-18-v2a-row-expand-design.md).
// line_src holds one source row filled by drawEyeRow().
// line_dst holds the horizontally-expanded row pushed through emitRow().
// Sized at compile time from the asset header + config.h.
uint16_t line_src[EYE_GALLERY_MAX_SCREEN_W];
uint16_t line_dst[RENDER_WIDTH];

EyeState eye;

uint32_t startTime;  // For FPS indicator; set in setup().

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("uncanny-eyes: boot");

  eye_side_init();
  initEyes();

  Serial.println("uncanny-eyes: display_begin()");
  display_begin();
  display_fillScreen(0x0000);
  display_setBrightness(DISPLAY_BRIGHTNESS);

  eye_gallery_touch_begin();

  eye_sync_init();

  startTime = millis();
  Serial.println("uncanny-eyes: running");
}

void loop() {
  eye_gallery_poll();
  eye_sync_tick();
  updateEye();
}
