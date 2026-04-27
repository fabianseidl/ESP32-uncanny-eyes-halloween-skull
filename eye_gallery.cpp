#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "eye_gallery.h"

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
static TouchDrvCST92xx s_touch;
static bool            s_touch_ok = false;
static bool            s_touch_down = false;
static uint32_t        s_touch_last_advance_ms = 0;

static void touch_poll_once(void) {
  if (!s_touch_ok) {
    return;
  }

  // CST9217 uses interrupt-driven reads: INT goes LOW when data is ready.
  // Calling getTouchPoints() without checking isPressed() first reads stale
  // registers and the buffer[6] ACK marker is absent → always returns empty.
  const bool pressed = s_touch.isPressed();
  if (!pressed) {
    s_touch_down = false;
    return;
  }

  const uint32_t       now = millis();
  const TouchPoints&   tp  = s_touch.getTouchPoints();
  const uint8_t        n   = tp.getPointCount();
  const bool           has = (n > 0);

#if EYE_GALLERY_TOUCH_LOG
  static bool s_prev_has = false;
  if (has != s_prev_has) {
    s_prev_has = has;
    Serial.print("eye_gallery: touch ");
    Serial.print(has ? "DOWN" : "UP");
    Serial.print(" points=");
    Serial.print(n);
    Serial.print(" TP_INT=");
    Serial.println(digitalRead(EYE_GALLERY_TP_INT));
    if (has && n > 0) {
      const TouchPoint& p = tp.getPoint(0);
      Serial.print("eye_gallery:   first x=");
      Serial.print(p.x);
      Serial.print(" y=");
      Serial.println(p.y);
    }
  }
#endif

  if (has) {
    if (!s_touch_down) {
      s_touch_down = true;
      if (now - s_touch_last_advance_ms > 450) {
        eye_gallery_next();
        s_touch_last_advance_ms = now;
#if EYE_GALLERY_TOUCH_LOG
        Serial.println("eye_gallery: touch -> style advance");
#endif
      }
    }
  } else {
    s_touch_down = false;
  }
}
#endif

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
#endif
}

void eye_gallery_poll(void) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N' || c == '\r' || c == '\n') {
      eye_gallery_next();
    }
  }

#if EYE_GALLERY_HAS_TOUCH
  touch_poll_once();
#endif
}
