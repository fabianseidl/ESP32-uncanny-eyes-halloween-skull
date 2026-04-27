#pragma once

#include <stdint.h>

#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2

struct eyeBlink {
  uint8_t  state;
  uint32_t duration;
  uint32_t startTime;
};

struct EyeState {
  eyeBlink blink;
};

extern EyeState eye;
extern uint32_t startTime;

void initEyes(void);
void updateEye(void);
