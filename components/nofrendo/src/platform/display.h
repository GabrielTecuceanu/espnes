#pragma once
#include <stdint.h>

void display_init(void);
void display_clear_black(void);
void display_push_frame(int x, int y, int w, int h, const uint16_t *rgb565);
// level 0-10, linear, min ~10% max 100%
void display_set_backlight(int level);
