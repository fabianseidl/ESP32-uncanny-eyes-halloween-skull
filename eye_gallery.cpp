#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "eye_gallery.h"
#include "eye_sync.h"

// Lewis He SensorLib — CST9217 touch driver.
// https://github.com/lewisxhe/SensorLib  (`arduino-cli lib install SensorLib`)
// EYE_GALLERY_HAS_TOUCH is set in config.h (default 1). arduino-cli resolves
// library paths from plain #include lines; __has_include() is invisible to its
// scanner and was causing SensorLib/src to never be added to the build path.
#if EYE_GALLERY_HAS_TOUCH
#include <TouchDrv.hpp>
#include "touch/TouchDrvCST92xx.h"
#endif

// Waveshare ESP32-S3-Touch-AMOLED-1.75 — docs/hardware-notes.md
#define EYE_GALLERY_TP_RESET 40
#define EYE_GALLERY_TP_INT   11
#define EYE_GALLERY_I2C_SDA  15
#define EYE_GALLERY_I2C_SCL  14
#define EYE_GALLERY_TOUCH_ADDR 0x5A

#ifndef EYE_GALLERY_TOUCH_LOG
#define EYE_GALLERY_TOUCH_LOG 0
#endif

static size_t s_gallery_idx = 0;

#if EYE_GALLERY_HAS_TOUCH
enum class GestureState {
  IDLE,
  PENDING,
  SWIPE_H,
  SWIPE_V,
  DONE
};

static TouchDrvCST92xx s_touch;
static bool            s_touch_ok = false;
static GestureState    s_gesture_state = GestureState::IDLE;
static int16_t         s_touch_x0, s_touch_y0;
static uint8_t         s_brightness_start;
static uint8_t         s_current_brightness = DISPLAY_BRIGHTNESS;
static uint32_t        s_last_brightness_broadcast_ms = 0;
static uint32_t        s_last_touch_ms = 0;

static bool s_pending_next = false;
static bool s_pending_prev = false;
static bool s_pending_brightness = false;

static void touch_poll_once(void) {
  if (!s_touch_ok) {
    return;
  }

  const bool     pressed = s_touch.isPressed();
  const uint32_t now     = millis();

  // Debounce the touch release: only reset to IDLE if no touch is detected for 250ms.
  // This handles jitter in isPressed() during swipes.
  if (!pressed) {
    if (now - s_last_touch_ms > 250) {
      if (s_gesture_state != GestureState::IDLE) {
#if EYE_GALLERY_TOUCH_LOG
        Serial.printf("eye_gallery: touch UP (debounced after %ums)\n", (unsigned)(now - s_last_touch_ms));
#endif
        s_gesture_state = GestureState::IDLE;
      }
    }
    return;
  }

  const TouchPoints& tp = s_touch.getTouchPoints();
  const uint8_t      n  = tp.getPointCount();
  if (n == 0) {
    return;
  }

  s_last_touch_ms     = now;
  const TouchPoint& p = tp.getPoint(0);

  if (s_gesture_state == GestureState::IDLE) {
    s_touch_x0         = p.x;
    s_touch_y0         = p.y;
    s_brightness_start = s_current_brightness;
    s_gesture_state    = GestureState::PENDING;
    return;
  }

  if (s_gesture_state == GestureState::DONE) {
    // Wait for release before next gesture.
    return;
  }

  const int16_t dx = p.x - s_touch_x0;
  const int16_t dy = p.y - s_touch_y0;

#if EYE_GALLERY_TOUCH_LOG
  static uint32_t s_last_trace_ms = 0;
  if (s_gesture_state == GestureState::PENDING && now - s_last_trace_ms > 100) {
    Serial.printf("eye_gallery: pending dx=%d dy=%d\n", (int)dx, (int)dy);
    s_last_trace_ms = now;
  }
#endif

  if (s_gesture_state == GestureState::PENDING) {
    if (abs(dx) > 20 || abs(dy) > 20) {
      if (abs(dx) > abs(dy)) {
        s_gesture_state = GestureState::SWIPE_H;
        if (dx > 0) {
          s_pending_next = true;
        } else {
          s_pending_prev = true;
        }
        // Discrete style switching: one change per swipe.
        s_gesture_state = GestureState::DONE;
#if EYE_GALLERY_TOUCH_LOG
        Serial.printf("eye_gallery: swipe style %s\n", dx > 0 ? "NEXT" : "PREV");
#endif
      } else {
        s_gesture_state = GestureState::SWIPE_V;
#if EYE_GALLERY_TOUCH_LOG
        Serial.println("eye_gallery: swipe brightness START");
#endif
      }
    }
    return;
  }

  if (s_gesture_state == GestureState::SWIPE_V) {
    // Smooth brightness dragging.
    // Map vertical travel to brightness range. ~300px for full range.
    // Drag UP (dy negative) -> Brighter; Drag DOWN (dy positive) -> Dimmer.
    int16_t b = (int16_t)s_brightness_start - (dy * 255 / 300);
    if (b < 10) {
      b = 10;
    }
    if (b > 255) {
      b = 255;
    }

    if ((uint8_t)b != s_current_brightness) {
      s_current_brightness = (uint8_t)b;
      s_pending_brightness = true;
    }
  }
}
#endif

void eye_gallery_apply_pending(void) {
#if EYE_GALLERY_HAS_TOUCH
  if (s_pending_next) {
    s_pending_next = false;
    eye_gallery_next();
  }
  if (s_pending_prev) {
    s_pending_prev = false;
    eye_gallery_prev();
  }
  if (s_pending_brightness) {
    s_pending_brightness = false;
    display_setBrightness(s_current_brightness);

    uint32_t now = millis();
    if (now - s_last_brightness_broadcast_ms > 50) {
#if EYE_SYNC_ENABLE
      eye_sync_broadcast_brightness(s_current_brightness);
#endif
      s_last_brightness_broadcast_ms = now;
    }
  }
#endif
}

void eye_gallery_init(void) {
  s_gallery_idx = 0;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: start ");
  Serial.println(eye_gallery[s_gallery_idx].name);
}

void eye_gallery_next(void) {
  s_gallery_idx = (s_gallery_idx + 1) % EYE_GALLERY_NUM;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: -> ");
  Serial.println(eye_gallery[s_gallery_idx].name);
#if EYE_SYNC_ENABLE
  eye_sync_broadcast_index((uint8_t)s_gallery_idx);
#endif
}

void eye_gallery_prev(void) {
  s_gallery_idx = (s_gallery_idx + EYE_GALLERY_NUM - 1) % EYE_GALLERY_NUM;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: <- ");
  Serial.println(eye_gallery[s_gallery_idx].name);
#if EYE_SYNC_ENABLE
  eye_sync_broadcast_index((uint8_t)s_gallery_idx);
#endif
}

void eye_gallery_apply_remote_index(uint8_t idx) {
  if (idx >= EYE_GALLERY_NUM) {
    return;  // ignore garbage from the wire
  }
  if ((size_t)idx == s_gallery_idx) {
    return;  // already in sync
  }
  s_gallery_idx = (size_t)idx;
  eye_renderer_set_active(&eye_gallery[s_gallery_idx]);
  Serial.print("eye_gallery: <- ");  // arrow distinguishes remote from local "->"
  Serial.println(eye_gallery[s_gallery_idx].name);
}

void eye_gallery_apply_remote_brightness(uint8_t brightness) {
#if EYE_GALLERY_HAS_TOUCH
  if (brightness == s_current_brightness) {
    return;
  }
  s_current_brightness = brightness;
#endif
  display_setBrightness(brightness);
}

void eye_gallery_touch_begin(void) {
#if !EYE_GALLERY_HAS_TOUCH
  Serial.println(
      "eye_gallery: touch disabled (install SensorLib: arduino-cli lib "
      "install SensorLib)");
#else
  pinMode(EYE_GALLERY_TP_RESET, OUTPUT);
  digitalWrite(EYE_GALLERY_TP_RESET, LOW);
  delay(30);
  digitalWrite(EYE_GALLERY_TP_RESET, HIGH);
  delay(50);

  s_touch.setPins(EYE_GALLERY_TP_RESET, EYE_GALLERY_TP_INT);
  s_touch_ok = s_touch.begin(Wire, EYE_GALLERY_TOUCH_ADDR, EYE_GALLERY_I2C_SDA,
                             EYE_GALLERY_I2C_SCL);
  if (!s_touch_ok) {
    Serial.println("eye_gallery: touch begin failed (serial n still works)");
    return;
  }
  // INT is active-low; weak pull-up helps a stable idle level when idle.
  pinMode(EYE_GALLERY_TP_INT, INPUT_PULLUP);

  s_touch.setMaxCoordinates(466, 466);
  s_touch.setMirrorXY(true, true);
  Serial.print("eye_gallery: touch ok ");
  Serial.println(s_touch.getModelName());
#endif
}

void eye_gallery_poll_touch_during_render(void) {
#if EYE_GALLERY_HAS_TOUCH
  touch_poll_once();
  eye_gallery_apply_pending();
#endif
}

void eye_gallery_check_fast_touch(void) {
#if EYE_GALLERY_HAS_TOUCH
  if (s_touch_ok && digitalRead(EYE_GALLERY_TP_INT) == LOW) {
    touch_poll_once();
  }
#endif
}

void eye_gallery_poll(void) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N') {
      eye_gallery_next();
    }
  }

#if EYE_GALLERY_HAS_TOUCH
  touch_poll_once();
  eye_gallery_apply_pending();
#endif
}
