#pragma once

#include <stdint.h>

// Resolved at boot from STA MAC (see EYE_SIDE_MAC_* in config.h).
extern uint8_t g_eye_side;

void eye_side_init(void);
