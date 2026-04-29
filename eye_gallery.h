#pragma once

#include "eye_runtime.h"

extern const EyeRuntime eye_gallery[];
extern const unsigned EYE_GALLERY_NUM;

void eye_renderer_set_active(const EyeRuntime* e);

void eye_gallery_init(void);
void eye_gallery_next(void);
/** Set the active gallery to the given index WITHOUT broadcasting.
 *  Used by the sync receive path to avoid re-triggering a network round trip. */
void eye_gallery_apply_remote_index(uint8_t idx);
void eye_gallery_poll(void);

/** CST9217 on the Waveshare AMOLED — call after `display_begin()` (Wire up). */
void eye_gallery_touch_begin(void);

/** Poll touch between animation frames (`split` can block `loop()` for a long time). */
void eye_gallery_poll_touch_during_render(void);
