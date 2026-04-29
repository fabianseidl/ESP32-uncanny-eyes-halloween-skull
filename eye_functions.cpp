// Scanline-streaming renderer for the Uncanny Eyes.
// Reads baked eye graphics (sclera / iris / upper / lower) from PROGMEM,
// computes per-pixel values, and pushes them through display_async in
// QSPI_ASYNC_CHUNK_PX-sized chunks. Single-eye, single-display variant.
//
// Originally written by Phil Burgess / Paint Your Dragon for Adafruit
// Industries (MIT license). Adapted to this project's single-eye Waveshare
// ESP32-S3 AMOLED target.
//
// Built as .cpp (not .ino) so Arduino does not inject prototypes before
// EyeRuntime is visible.

#include <Arduino.h>
#include "config.h"
#include "eyes.h"
#include "eye_runtime.h"
#include "eye_gallery.h"
#include "eye_sync.h"

// Active asset (runtime gallery index); set before first render.
static const EyeRuntime* g_eye = nullptr;

void eye_renderer_set_active(const EyeRuntime* e) {
  g_eye = e;
}

// Autonomous iris motion uses a fractal behavior to simulate both the
// major reaction of the eye plus the continuous smaller adjustments.
uint16_t oldIris = 110, newIris = 110;

// Row-expand line buffers (defined in ESP32-uncanny-eyes-halloween-skull.ino).
extern uint16_t line_src[];
extern uint16_t line_dst[];

#include "display_async.h"
#include "display_sketch.h"

static uint16_t s_chunk_buf[QSPI_ASYNC_CHUNK_PX];
static uint32_t s_chunk_fill = 0;

static void drawEyeRow(uint32_t sy, uint32_t scleraXsave, uint32_t scleraY,
                       int32_t irisY, uint32_t iScale,
                       uint32_t uT, uint32_t lT);
static void expandRow(const uint16_t* src, uint16_t* dst, uint16_t screen_w);
static void emitRow(const uint16_t* dst);
static void emitRowFlushTail();
static void drawEye(uint32_t iScale, uint32_t scleraX, uint32_t scleraY,
                    uint32_t uT, uint32_t lT);
static void frame(uint16_t iScale);
static void split(int16_t startValue, int16_t endValue, uint32_t startTime_local,
                  int32_t duration, int16_t range);

void initEyes(void) {
  eye_gallery_init();
  oldIris = (g_eye->iris_min + g_eye->iris_max) / 2;
  Serial.println("initEyes: runtime gallery v1");
  eye.blink.state = NOBLINK;
}

void updateEye(void) {
  newIris = random((long)g_eye->iris_min, (long)g_eye->iris_max);
  split(oldIris, newIris, micros(), 10000000L,
        g_eye->iris_max - g_eye->iris_min);
  oldIris = newIris;
}

static void drawEye(uint32_t iScale, uint32_t scleraX, uint32_t scleraY,
                    uint32_t uT, uint32_t lT) {
  display_setAddrWindow(0, 0, RENDER_WIDTH, RENDER_HEIGHT);

  display_pixelsBegin();

  const uint32_t scleraXsave = scleraX;
  int32_t        irisY       = (int32_t)scleraY -
                         (g_eye->sclera_height - g_eye->iris_height) / 2;

  int32_t vAccum = 0;
  for (uint32_t sy = 0; sy < g_eye->screen_h; sy++) {
    drawEyeRow(sy, scleraXsave, scleraY + sy, irisY + (int32_t)sy,
               iScale, uT, lT);
    expandRow(line_src, line_dst, g_eye->screen_w);
    emitRow(line_dst);
    vAccum += (int32_t)RENDER_HEIGHT - (int32_t)g_eye->screen_h;
    while (vAccum >= (int32_t)g_eye->screen_h) {
      vAccum -= g_eye->screen_h;
      emitRow(line_dst);
    }
  }

  emitRowFlushTail();
  display_pixelsEnd();
}

static void drawEyeRow(uint32_t sy, uint32_t scleraXsave, uint32_t scleraY,
                       int32_t irisY, uint32_t iScale,
                       uint32_t uT, uint32_t lT) {
  const uint16_t lidX_start =
      (EYE_SIDE == EYE_SIDE_LEFT) ? (g_eye->screen_w - 1) : 0;
  const int16_t lidX_step = (EYE_SIDE == EYE_SIDE_LEFT) ? -1 : 1;

  uint32_t scleraX = scleraXsave;
  int32_t  irisX =
      (int32_t)scleraXsave - (g_eye->sclera_width - g_eye->iris_width) / 2;
  uint16_t lidX = lidX_start;

  const uint8_t* const lower_row =
      g_eye->lower + sy * g_eye->screen_w;
  const uint8_t* const upper_row =
      g_eye->upper + sy * g_eye->screen_w;
  const uint16_t* const sclera_row =
      g_eye->sclera + scleraY * g_eye->sclera_width;
  const bool irisYok =
      (irisY >= 0) && (irisY < g_eye->iris_height);
  const uint16_t* const polar_row = irisYok
                                        ? (g_eye->polar + irisY * g_eye->iris_width)
                                        : nullptr;

  for (uint32_t sx = 0; sx < g_eye->screen_w;
       sx++, scleraX++, irisX++, lidX += lidX_step) {
    uint32_t p, a, d;
    if ((pgm_read_byte(lower_row + lidX) <= lT) ||
        (pgm_read_byte(upper_row + lidX) <= uT)) {
      p = 0;
    } else if (!irisYok || (irisX < 0) || (irisX >= g_eye->iris_width)) {
      p = pgm_read_word(sclera_row + scleraX);
    } else {
      p = pgm_read_word(polar_row + irisX);
      d = (iScale * (p & 0x7F)) / 128;
      if (d < g_eye->iris_map_height) {
        a = (g_eye->iris_map_width * (p >> 7)) / 512;
        p = pgm_read_word(g_eye->iris + d * g_eye->iris_map_width + a);
      } else {
        p = pgm_read_word(sclera_row + scleraX);
      }
    }
    line_src[sx] = (uint16_t)p;
  }
}

static void expandRow(const uint16_t* src, uint16_t* dst, uint16_t screen_w) {
  uint16_t sx     = 0;
  int32_t  hAccum = 0;
  for (uint16_t rx = 0; rx < RENDER_WIDTH; rx++) {
    dst[rx] = src[sx];
    hAccum += screen_w;
    while (hAccum >= (int32_t)RENDER_WIDTH) {
      hAccum -= RENDER_WIDTH;
      sx++;
    }
  }
}

static void emitRow(const uint16_t* dst) {
  for (uint32_t i = 0; i < RENDER_WIDTH; i++) {
    s_chunk_buf[s_chunk_fill++] = dst[i];
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

static const uint8_t ease[] = {
  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,
  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  7,  8,  9,  9, 10, 10,
  11, 12, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23,
  24, 25, 26, 27, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 57, 58,
  60, 61, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80,
  81, 83, 84, 85, 87, 88, 90, 91, 93, 94, 96, 97, 98, 100, 101, 103,
  104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 119, 121, 122, 124, 125, 127,
  128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143, 145, 146, 148, 149, 151,
  152, 154, 155, 157, 158, 159, 161, 162, 164, 165, 167, 168, 170, 171, 172, 174,
  175, 177, 178, 179, 181, 182, 183, 185, 186, 188, 189, 190, 192, 193, 194, 195,
  197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210, 211, 213, 214, 215,
  216, 217, 218, 219, 220, 221, 222, 224, 225, 226, 227, 228, 228, 229, 230, 231,
  232, 233, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244,
  245, 245, 246, 246, 247, 248, 248, 249, 249, 250, 250, 251, 251, 251, 252, 252,
  252, 253, 253, 253, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255
};

#ifdef AUTOBLINK
uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L;
#endif

static void frame(uint16_t iScale) {
  // `split()` can call `frame()` many times before `loop()` returns — touch
  // and sync must be polled here, not only from `loop()`, or events are
  // missed and RX latency balloons (split runs for up to ~10 s per call).
  eye_gallery_poll_touch_during_render();
  eye_sync_tick();

  static uint32_t frames = 0;
  int16_t         eyeX, eyeY;
  uint32_t        t = micros();

  if (!(++frames & 255)) {
    float elapsed = (millis() - startTime) / 1000.0f;
    if (elapsed > 0) {
      Serial.print("FPS=");
      Serial.println((uint16_t)(frames / elapsed));
    }
  }

  static bool     eyeInMotion      = false;
  static int16_t  eyeOldX = 512, eyeOldY = 512, eyeNewX = 512, eyeNewY = 512;
  static uint32_t eyeMoveStartTime = 0L;
  static int32_t  eyeMoveDuration  = 0L;

  int32_t dt = t - eyeMoveStartTime;
  if (eyeInMotion) {
    if (dt >= eyeMoveDuration) {
      eyeInMotion      = false;
      eyeMoveDuration  = random(3000000);
      eyeMoveStartTime = t;
      eyeX = eyeOldX = eyeNewX;
      eyeY = eyeOldY = eyeNewY;
    } else {
      int16_t eased = ease[255 * dt / eyeMoveDuration] + 1;
      eyeX = eyeOldX + (((eyeNewX - eyeOldX) * eased) / 256);
      eyeY = eyeOldY + (((eyeNewY - eyeOldY) * eased) / 256);
    }
  } else {
    eyeX = eyeOldX;
    eyeY = eyeOldY;
    if (dt > eyeMoveDuration) {
      int16_t  dx, dy;
      uint32_t d2;
      do {
        eyeNewX = random(1024);
        eyeNewY = random(1024);
        dx      = (eyeNewX * 2) - 1023;
        dy      = (eyeNewY * 2) - 1023;
      } while ((d2 = (dx * dx + dy * dy)) > (1023 * 1023));
      eyeMoveDuration  = random(72000, 144000);
      eyeMoveStartTime = t;
      eyeInMotion      = true;
    }
  }

#ifdef AUTOBLINK
  if ((t - timeOfLastBlink) >= timeToNextBlink) {
    timeOfLastBlink = t;
    uint32_t blinkDuration = random(36000, 72000);
    if (eye.blink.state == NOBLINK) {
      eye.blink.state     = ENBLINK;
      eye.blink.startTime = t;
      eye.blink.duration  = blinkDuration;
    }
    timeToNextBlink = blinkDuration * 3 + random(4000000);
  }
#endif

  if (eye.blink.state) {
    if ((t - eye.blink.startTime) >= eye.blink.duration) {
      if (++eye.blink.state > DEBLINK) {
        eye.blink.state = NOBLINK;
      } else {
        eye.blink.duration *= 2;
        eye.blink.startTime = t;
      }
    }
  }

  eyeX = map(eyeX, 0, 1023, 0, g_eye->sclera_width - g_eye->screen_w);
  eyeY = map(eyeY, 0, 1023, 0, g_eye->sclera_height - g_eye->screen_h);

  eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4;
  if (eyeX > (g_eye->sclera_width - g_eye->screen_w)) {
    eyeX = g_eye->sclera_width - g_eye->screen_w;
  }
  if (eyeX < 0) eyeX = 0;

  static uint8_t uThreshold = 128;
  uint8_t        lThreshold, n;
#ifdef TRACKING
  int16_t sampleX = g_eye->sclera_width / 2 - (eyeX / 2);
  int16_t sampleY =
      g_eye->sclera_height / 2 - (eyeY + g_eye->iris_height / 4);
  if (sampleY < 0) {
    n = 0;
  } else {
    n = (pgm_read_byte(g_eye->upper + sampleY * g_eye->screen_w + sampleX) +
         pgm_read_byte(g_eye->upper + sampleY * g_eye->screen_w +
                       (g_eye->screen_w - 1 - sampleX))) /
        2;
  }
  uThreshold = (uThreshold * 3 + n) / 4;
  lThreshold = 254 - uThreshold;
#else
  uThreshold = lThreshold = 0;
#endif

  if (eye.blink.state) {
    uint32_t s = (t - eye.blink.startTime);
    if (s >= eye.blink.duration) {
      s = 255;
    } else {
      s = 255 * s / eye.blink.duration;
    }
    s = (eye.blink.state == DEBLINK) ? 1 + s : 256 - s;
    n          = (uThreshold * s + 254 * (257 - s)) / 256;
    lThreshold = (lThreshold * s + 254 * (257 - s)) / 256;
  } else {
    n = uThreshold;
  }

  drawEye(iScale, eyeX, eyeY, n, lThreshold);
}

static void split(int16_t startValue, int16_t endValue, uint32_t startTime_local,
                  int32_t duration, int16_t range) {
  if (range >= 8) {
    range    /= 2;
    duration /= 2;
    int16_t  midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime  = startTime_local + duration;
    split(startValue, midValue, startTime_local, duration, range);
    split(midValue, endValue, midTime, duration, range);
  } else {
    int32_t dt;
    int16_t v;
    while ((dt = (micros() - startTime_local)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if (v < g_eye->iris_min) v = g_eye->iris_min;
      else if (v > g_eye->iris_max) v = g_eye->iris_max;
      frame(v);
    }
  }
}
