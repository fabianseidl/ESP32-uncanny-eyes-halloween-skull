#pragma once

#include "eye_runtime.h"

extern const EyeRuntime eye_gallery[];
extern const unsigned EYE_GALLERY_NUM;

void eye_renderer_set_active(const EyeRuntime* e);

void eye_gallery_init(void);
void eye_gallery_next(void);
void eye_gallery_poll(void);
