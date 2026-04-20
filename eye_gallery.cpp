#include <Arduino.h>
#include <Wire.h>

#include "eye_gallery.h"

// Lewis He SensorLib — same CST9217 path as Waveshare 06_LVGL_Widgets.ino.
// https://github.com/lewisxhe/SensorLib  (`arduino-cli lib install SensorLib`)
#if __has_include("touch/TouchDrvCST92xx.h")
#include "touch/TouchDrvCST92xx.h"
#define EYE_GALLERY_HAS_TOUCH 1
#else
#define EYE_GALLERY_HAS_TOUCH 0
#endif

// Waveshare ESP32-S3-Touch-AMOLED-1.75 — docs/hardware-notes.md
#define EYE_GALLERY_TP_RESET 40
#define EYE_GALLERY_TP_INT   11
#define EYE_GALLERY_I2C_SDA  15
#define EYE_GALLERY_I2C_SCL  14
#define EYE_GALLERY_TOUCH_ADDR 0x5A

static size_t s_gallery_idx = 0;

#if EYE_GALLERY_HAS_TOUCH
static TouchDrvCST92xx s_touch;
static bool            s_touch_ok      = false;
static bool            s_touch_pressed = false;
static uint32_t        s_touch_last_advance_ms = 0;
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
  s_touch.setMaxCoordinates(466, 466);
  s_touch.setMirrorXY(true, true);
  Serial.print("eye_gallery: touch ok ");
  Serial.println(s_touch.getModelName());
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
  if (!s_touch_ok) {
    return;
  }
  const bool   has = s_touch.getTouchPoints().hasPoints();
  const uint32_t now = millis();
  if (has) {
    s_touch_pressed = true;
  } else if (s_touch_pressed) {
    s_touch_pressed = false;
    if (now - s_touch_last_advance_ms > 400) {
      eye_gallery_next();
      s_touch_last_advance_ms = now;
    }
  }
#endif
}
