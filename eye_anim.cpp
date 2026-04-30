#include <Arduino.h>

#include "config.h"
#include "eye_anim.h"

#if EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE

#include "eyes.h"
#include "eye_side.h"

#ifdef AUTOBLINK
extern uint32_t timeOfLastBlink;
extern uint32_t timeToNextBlink;
#endif

#ifndef EYE_SYNC_ANIM_LOG
#define EYE_SYNC_ANIM_LOG 0
#endif

// --- LCG mixing (one chain step per logical pulse; expand via mix32) --------
static uint32_t s_lcg = 1u;

static uint32_t anim_lcg_step(void) {
  s_lcg = s_lcg * 1664525u + 1013904223u;
  return s_lcg;
}

static uint32_t mix32(uint32_t x) {
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static int32_t span_umod(uint32_t* w, int32_t lo, int32_t hi) {
  if (hi <= lo) {
    return lo;
  }
  uint32_t span  = (uint32_t)(hi - lo);
  int32_t  pick  = lo + (int32_t)((*w) % span);
  *w             = mix32(*w);
  return pick;
}

// --- Gaze FSM (pulse-driven randomness) ------------------------------------
// All state transitions happen in pulse_fsm_one(); frame_gaze() is pure
// interpolation. Motion duration is counted in pulses so both boards transition
// state on the same pulse step regardless of clock skew or RX latency.
static bool     s_eye_in_motion      = false;
static int16_t  s_eye_old_x = 512;
static int16_t  s_eye_old_y = 512;
static int16_t  s_eye_new_x = 512;
static int16_t  s_eye_new_y = 512;
static uint32_t s_eye_move_start_us = 0;
static int32_t  s_eye_move_duration_us = 0;
static int32_t  s_idle_pulses_left   = 0;
static bool     s_need_idle_pick     = true;
static int32_t  s_motion_pulses_total = 0;
static uint32_t s_motion_start_step   = 0;

static uint32_t s_last_applied_step  = 0;
static uint32_t s_last_pulse_seen_ms = 0;
static uint32_t s_epoch_wall_ms      = 0;
static bool     s_follower_saw_pulse = false;

#ifdef AUTOBLINK
static int32_t s_blink_wait_pulses = 12;
#endif

void eye_anim_init(void) {
  s_lcg                = 1u;
  s_eye_in_motion      = false;
  s_eye_old_x = s_eye_new_x = 512;
  s_eye_old_y = s_eye_new_y = 512;
  s_eye_move_start_us  = micros();
  s_eye_move_duration_us = 0;
  s_idle_pulses_left   = 0;
  s_need_idle_pick     = true;
  s_motion_pulses_total = 0;
  s_motion_start_step   = 0;
  s_last_applied_step  = 0;
  s_last_pulse_seen_ms = millis();
  s_epoch_wall_ms      = millis();
  s_follower_saw_pulse = (g_eye_side == EYE_SIDE_LEFT);
#ifdef AUTOBLINK
  s_blink_wait_pulses = 12;
#endif
}

void eye_anim_reset_epoch(uint32_t anim_seed) {
  randomSeed(anim_seed);
  s_lcg = anim_seed ^ 0x9E3779B9u;
  if (s_lcg == 0u) {
    s_lcg = 0xA5A5A5A5u;
  }
  s_eye_in_motion = false;
  s_eye_old_x = s_eye_new_x = 512;
  s_eye_old_y = s_eye_new_y = 512;
  s_eye_move_start_us     = micros();
  s_eye_move_duration_us  = 0;
  s_idle_pulses_left      = 0;
  s_need_idle_pick        = true;
  s_motion_pulses_total   = 0;
  s_motion_start_step     = 0;
  s_last_applied_step  = 0;
  s_last_pulse_seen_ms = millis();
  s_epoch_wall_ms      = millis();
  s_follower_saw_pulse = false;
#ifdef AUTOBLINK
  s_blink_wait_pulses     = 12;
  timeOfLastBlink         = micros();
  timeToNextBlink         = 0;
  eye.blink.state         = NOBLINK;
#endif
#if EYE_SYNC_ANIM_LOG
  Serial.printf("eye_anim: epoch seed=0x%08X\n", (unsigned)anim_seed);
#endif
}

bool eye_anim_is_degraded(void) {
  if (g_eye_side == EYE_SIDE_LEFT) {
    return false;
  }
  if (!s_follower_saw_pulse) {
    return (millis() - s_epoch_wall_ms) > 1500u;
  }
  uint32_t age = (uint32_t)(millis() - s_last_pulse_seen_ms);
  return age > (uint32_t)EYE_SYNC_ANIM_FALLBACK_MS;
}

bool eye_anim_use_sync_motion(void) {
  return !eye_anim_is_degraded();
}

static void pulse_fsm_one(void) {
  uint32_t w = anim_lcg_step();

  // Motion-end transition is pulse-counted (deterministic on both boards),
  // never wall-time. frame_gaze() never mutates FSM state.
  if (s_eye_in_motion &&
      (s_last_applied_step - s_motion_start_step) >=
          (uint32_t)s_motion_pulses_total) {
    s_eye_in_motion  = false;
    s_eye_old_x      = s_eye_new_x;
    s_eye_old_y      = s_eye_new_y;
    s_need_idle_pick = true;
  }

  if (s_need_idle_pick) {
    s_idle_pulses_left =
        span_umod(&w, 15, 36);  // ~1.5–3.5 s at 100 ms pulse
    s_need_idle_pick = false;
  }

  if (!s_eye_in_motion) {
    if (s_idle_pulses_left > 0) {
      s_idle_pulses_left--;
    } else {
      int16_t nx, ny;
      uint32_t d2;
      int      guard = 0;
      do {
        nx = (int16_t)span_umod(&w, 0, 1024);
        ny = (int16_t)span_umod(&w, 0, 1024);
        int16_t dx = (nx * 2) - 1023;
        int16_t dy = (ny * 2) - 1023;
        d2         = (uint32_t)((int32_t)dx * dx + (int32_t)dy * dy);
        if (++guard > 64) {
          nx = 512;
          ny = 512;
          break;
        }
      } while (d2 > (uint32_t)(1023 * 1023));

      s_eye_new_x             = nx;
      s_eye_new_y             = ny;
      // Duration in pulses (1–2 at 100 ms) so motion always ends on a pulse
      // boundary on both boards. micros() value is for interpolation only.
      s_motion_pulses_total   = span_umod(&w, 1, 3);
      s_motion_start_step     = s_last_applied_step;
      s_eye_move_duration_us  = (int32_t)s_motion_pulses_total *
                                (int32_t)EYE_SYNC_ANIM_PULSE_MS * 1000;
      s_eye_move_start_us     = micros();
      s_eye_in_motion         = true;
    }
  }

#ifdef AUTOBLINK
  if (eye.blink.state == NOBLINK) {
    if (s_blink_wait_pulses > 0) {
      s_blink_wait_pulses--;
    }
    if (s_blink_wait_pulses <= 0) {
      uint32_t blink_dur = (uint32_t)span_umod(&w, 36000, 72001);
      timeOfLastBlink        = micros();
      eye.blink.state        = ENBLINK;
      eye.blink.startTime    = micros();
      eye.blink.duration     = blink_dur;
      s_blink_wait_pulses    = span_umod(&w, 30, 121);  // ~3–12 s before next
      timeToNextBlink        = 0;
    }
  }
#endif
}

void eye_anim_on_pulse(uint32_t step) {
  s_last_pulse_seen_ms = millis();
  s_follower_saw_pulse = true;

  if (step == 0u) {
    return;
  }
  // Duplicate RX or stale packet: do not underflow (step - last_applied).
  if (step <= s_last_applied_step) {
    return;
  }

  const uint32_t max_catch = 512u;
  uint32_t       behind    = step - s_last_applied_step;

  if (behind > max_catch) {
#if EYE_SYNC_ANIM_LOG
    Serial.printf("eye_anim: resync large gap last=%u step=%u\n",
                  (unsigned)s_last_applied_step, (unsigned)step);
#endif
    s_last_applied_step = step;
    pulse_fsm_one();
    return;
  }

  while (s_last_applied_step < step) {
    s_last_applied_step++;
    pulse_fsm_one();
  }
}

static const uint8_t k_ease[] = {
  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  7,  8,  9,  9, 10, 10,
  11, 12, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 57, 58, 60, 61, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80,
  81, 83, 84, 85, 87, 88, 90, 91, 93, 94, 96, 97, 98, 100, 101, 103, 104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 119, 121, 122, 124, 125, 127,
  128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143, 145, 146, 148, 149, 151, 152, 154, 155, 157, 158, 159, 161, 162, 164, 165, 167, 168, 170, 171, 172, 174,
  175, 177, 178, 179, 181, 182, 183, 185, 186, 188, 189, 190, 192, 193, 194, 195, 197, 198, 199, 201, 202, 203, 204, 205, 207, 208, 209, 210, 211, 213, 214, 215,
  216, 217, 218, 219, 220, 221, 222, 224, 225, 226, 227, 228, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244,
  245, 245, 246, 246, 247, 248, 248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 252, 253, 253, 253, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255
};

void eye_anim_frame_gaze(uint32_t t, int16_t* out_x, int16_t* out_y) {
  int32_t dt = (int32_t)(t - s_eye_move_start_us);
  int16_t eyeX, eyeY;

  if (s_eye_in_motion && s_eye_move_duration_us > 0 &&
      dt < s_eye_move_duration_us) {
    if (dt < 0) dt = 0;
    int16_t eased = (int16_t)(k_ease[(uint32_t)(255 * dt / s_eye_move_duration_us)] + 1);
    eyeX = (int16_t)(s_eye_old_x +
                     (((int32_t)(s_eye_new_x - s_eye_old_x) * eased) / 256));
    eyeY = (int16_t)(s_eye_old_y +
                     (((int32_t)(s_eye_new_y - s_eye_old_y) * eased) / 256));
  } else if (s_eye_in_motion) {
    // Past local duration but pulse-driven motion-end hasn't fired yet:
    // hold at the new target until pulse_fsm_one() flips state.
    eyeX = s_eye_new_x;
    eyeY = s_eye_new_y;
  } else {
    eyeX = s_eye_old_x;
    eyeY = s_eye_old_y;
  }
  *out_x = eyeX;
  *out_y = eyeY;
}

#else  // !(EYE_SYNC_ENABLE && EYE_SYNC_ANIM_ENABLE)

void eye_anim_init(void) {}

#endif
