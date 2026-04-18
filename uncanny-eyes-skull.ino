// Uncanny Eyes -- Waveshare ESP32-S3-Touch-AMOLED-1.75 port (v1).
//
// Renders one eye (EYE_SIDE in config.h) as a 240x240 image centered on
// the 466x466 CO5300 AMOLED. See docs/superpowers/specs for the design.

#include "config.h"

// Ping-pong pixel buffers drained by display_writePixels(). Shared with
// eye_functions.ino.
#define BUFFER_SIZE 1024
#define BUFFERS 2
uint16_t pbuffer[BUFFERS][BUFFER_SIZE];
bool dmaBuf = 0;

// Blink state machine shared with eye_functions.ino.
#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2
struct eyeBlink {
  uint8_t  state;
  uint32_t duration;
  uint32_t startTime;
};

// One eye in v1.
struct EyeState {
  eyeBlink blink;
};
EyeState eye;

uint32_t startTime;  // For FPS indicator; set in setup().

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("uncanny-eyes: boot");

  initEyes();

  Serial.println("uncanny-eyes: display_begin()");
  display_begin();
  display_fillScreen(0x0000);
  display_setBrightness(DISPLAY_BRIGHTNESS);

  startTime = millis();
  Serial.println("uncanny-eyes: running");
}

void loop() {
  updateEye();
}
