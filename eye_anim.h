#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

void eye_anim_init(void);

#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE

void eye_anim_reset_epoch(uint32_t anim_seed);
void eye_anim_on_pulse(uint32_t step);
bool eye_anim_is_degraded(void);
bool eye_anim_use_sync_motion(void);
void eye_anim_frame_gaze(uint32_t t_micros, int16_t* out_eyeX, int16_t* out_eyeY);

#else

static inline void eye_anim_reset_epoch(uint32_t anim_seed) {
  (void)anim_seed;
}
static inline void eye_anim_on_pulse(uint32_t step) {
  (void)step;
}
static inline bool eye_anim_is_degraded(void) {
  return true;
}
static inline bool eye_anim_use_sync_motion(void) {
  return false;
}
static inline void eye_anim_frame_gaze(uint32_t t_micros, int16_t* out_eyeX,
                                       int16_t* out_eyeY) {
  (void)t_micros;
  *out_eyeX = 512;
  *out_eyeY = 512;
}

#endif

#ifdef __cplusplus
}
#endif
