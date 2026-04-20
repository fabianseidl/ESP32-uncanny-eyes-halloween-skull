#pragma once

#include <stdint.h>

struct EyeRuntime {
  const char* name;
  uint16_t screen_w, screen_h;
  uint16_t sclera_width, sclera_height;
  uint16_t iris_width, iris_height;
  uint16_t iris_map_width, iris_map_height;
  int16_t iris_min, iris_max;
  const uint16_t* sclera;
  const uint16_t* iris;
  const uint8_t* upper;
  const uint8_t* lower;
  const uint16_t* polar;
};
