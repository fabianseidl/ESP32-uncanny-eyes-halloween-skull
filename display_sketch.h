#pragma once

#include <stdint.h>

void display_begin(void);
void display_setBrightness(uint8_t value);
void display_fillScreen(uint16_t color);
void display_setAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h);
