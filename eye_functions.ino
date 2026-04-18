// Scanline-streaming renderer for the Uncanny Eyes.
// Reads baked eye graphics (sclera / iris / upper / lower) from PROGMEM,
// computes per-pixel values, and pushes them through display_writePixels()
// in 1024-pixel chunks. Single-eye, single-display variant.
//
// Originally written by Phil Burgess / Paint Your Dragon for Adafruit
// Industries (MIT license). Adapted to this project's single-eye Waveshare
// ESP32-S3 AMOLED target.

// Autonomous iris motion uses a fractal behavior to simulate both the
// major reaction of the eye plus the continuous smaller adjustments.
uint16_t oldIris = (IRIS_MIN + IRIS_MAX) / 2, newIris;

// Initialise eye ----------------------------------------------------------
void initEyes(void) {
  Serial.println("initEyes: single eye v1");
  eye.blink.state = NOBLINK;
}

// UPDATE EYE --------------------------------------------------------------
void updateEye(void) {
  newIris = random(IRIS_MIN, IRIS_MAX);
  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}

// EYE-RENDERING FUNCTION --------------------------------------------------
void drawEye( // Renders the eye. Inputs must be pre-clipped & valid.
    uint32_t iScale,   // Scale factor for iris
    uint32_t scleraX,  // First pixel X offset into sclera image
    uint32_t scleraY,  // First pixel Y offset into sclera image
    uint32_t uT,       // Upper eyelid threshold value
    uint32_t lT) {     // Lower eyelid threshold value
  uint32_t screenX, screenY, scleraXsave;
  int32_t  irisX, irisY;
  uint32_t p, a, d;
  uint32_t pixels = 0;

  display_startWrite();
  display_setAddrWindow(EYE_RENDER_X, EYE_RENDER_Y,
                        EYE_RENDER_WIDTH, EYE_RENDER_HEIGHT);

  // Eyelid map direction depends on which side this board renders.
  // LEFT eye walks the eyelid map right-to-left (caruncle on the
  // nose-side); RIGHT eye walks it left-to-right.
  const uint16_t lidX_start = (EYE_SIDE == EYE_SIDE_LEFT) ? (SCREEN_WIDTH - 1) : 0;
  const int16_t  lidX_step  = (EYE_SIDE == EYE_SIDE_LEFT) ? -1 : 1;

  scleraXsave = scleraX;
  irisY       = scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;

  for (screenY = 0; screenY < SCREEN_HEIGHT; screenY++, scleraY++, irisY++) {
    scleraX = scleraXsave;
    irisX   = scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    uint16_t lidX = lidX_start;
    for (screenX = 0; screenX < SCREEN_WIDTH;
         screenX++, scleraX++, irisX++, lidX += lidX_step) {
      if ((pgm_read_byte(lower + screenY * SCREEN_WIDTH + lidX) <= lT) ||
          (pgm_read_byte(upper + screenY * SCREEN_WIDTH + lidX) <= uT)) {
        // Covered by eyelid
        p = 0;
      } else if ((irisY < 0) || (irisY >= IRIS_HEIGHT) ||
                 (irisX < 0) || (irisX >= IRIS_WIDTH)) {
        // In sclera
        p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);
      } else {
        // Maybe iris...
        p = pgm_read_word(polar + irisY * IRIS_WIDTH + irisX);
        d = (iScale * (p & 0x7F)) / 128;
        if (d < IRIS_MAP_HEIGHT) {
          a = (IRIS_MAP_WIDTH * (p >> 7)) / 512;
          p = pgm_read_word(iris + d * IRIS_MAP_WIDTH + a);
        } else {
          p = pgm_read_word(sclera + scleraY * SCLERA_WIDTH + scleraX);
        }
      }
      // Arduino_GFX's writePixels() takes host-order RGB565 and swaps
      // to bus byte order internally -- do NOT pre-swap here.
      *(&pbuffer[dmaBuf][0] + pixels++) = p;

      if (pixels >= BUFFER_SIZE) {
        yield();
        display_writePixels(&pbuffer[dmaBuf][0], pixels);
        dmaBuf = !dmaBuf;
        pixels = 0;
      }
    }
  }

  if (pixels) {
    display_writePixels(&pbuffer[dmaBuf][0], pixels);
  }
  display_endWrite();
}

// EYE ANIMATION -----------------------------------------------------------

const uint8_t ease[] = { // Ease in/out curve for eye movements 3*t^2-2*t^3
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

// Process motion for a single frame. Takes the iris scale value and drives
// the full animation -> render pipeline.
void frame(uint16_t iScale) {
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

  // --- Autonomous X/Y eye motion ---
  // Periodically initiates motion to a new random point, random speed,
  // holds there for random period until next motion.
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

  // --- Autonomous blinking ---
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

  // Advance blink state machine.
  if (eye.blink.state) {
    if ((t - eye.blink.startTime) >= eye.blink.duration) {
      if (++eye.blink.state > DEBLINK) {
        eye.blink.state = NOBLINK;
      } else {
        eye.blink.duration *= 2;   // DEBLINK is 1/2 ENBLINK speed
        eye.blink.startTime = t;
      }
    }
  }

  // --- Map to pixel units ---
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH  - SCREEN_WIDTH);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - SCREEN_HEIGHT);

  // Slight convergence so a pair of eyes looks fixated. +/- 4 px nudge
  // based on which side this board drives.
  eyeX += (EYE_SIDE == EYE_SIDE_LEFT) ? -4 : 4;
  if (eyeX > (SCLERA_WIDTH - SCREEN_WIDTH)) eyeX = SCLERA_WIDTH - SCREEN_WIDTH;
  if (eyeX < 0) eyeX = 0;

  // --- Eyelid tracking ---
  static uint8_t uThreshold = 128;
  uint8_t        lThreshold, n;
#ifdef TRACKING
  int16_t sampleX = SCLERA_WIDTH  / 2 - (eyeX / 2);
  int16_t sampleY = SCLERA_HEIGHT / 2 - (eyeY + IRIS_HEIGHT / 4);
  if (sampleY < 0) {
    n = 0;
  } else {
    n = (pgm_read_byte(upper + sampleY * SCREEN_WIDTH + sampleX) +
         pgm_read_byte(upper + sampleY * SCREEN_WIDTH + (SCREEN_WIDTH - 1 - sampleX))) / 2;
  }
  uThreshold = (uThreshold * 3 + n) / 4;
  lThreshold = 254 - uThreshold;
#else
  uThreshold = lThreshold = 0;
#endif

  // Mix threshold with current blink position.
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

// AUTONOMOUS IRIS SCALING -------------------------------------------------

void split( // Subdivides motion path into two sub-paths w/ randomization
    int16_t  startValue,
    int16_t  endValue,
    uint32_t startTime_local,
    int32_t  duration,
    int16_t  range) {
  if (range >= 8) {
    range    /= 2;
    duration /= 2;
    int16_t  midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime  = startTime_local + duration;
    split(startValue, midValue, startTime_local, duration, range);
    split(midValue,   endValue, midTime,         duration, range);
  } else {
    int32_t dt;
    int16_t v;
    while ((dt = (micros() - startTime_local)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if (v < IRIS_MIN)      v = IRIS_MIN;
      else if (v > IRIS_MAX) v = IRIS_MAX;
      frame(v);
    }
  }
}
