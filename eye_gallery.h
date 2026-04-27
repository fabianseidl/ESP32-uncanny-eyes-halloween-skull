#pragma once

#include "eye_runtime.h"

extern const EyeRuntime eye_gallery[];
extern const unsigned EYE_GALLERY_NUM;

void eye_renderer_set_active(const EyeRuntime* e);

void eye_gallery_init(void);
void eye_gallery_next(void);
void eye_gallery_poll(void);

/** CST9217 on the Waveshare AMOLED — call after `display_begin()` (Wire up). */
void eye_gallery_touch_begin(void);

/** Poll touch between animation frames (`split` can block `loop()` for a long time). */
void eye_gallery_poll_touch_during_render(void);
