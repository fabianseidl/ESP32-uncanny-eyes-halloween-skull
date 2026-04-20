#include <Arduino.h>

#include "eye_gallery.h"

static size_t s_gallery_idx = 0;

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

void eye_gallery_poll(void) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N' || c == '\r' || c == '\n') {
      eye_gallery_next();
    }
  }
}
