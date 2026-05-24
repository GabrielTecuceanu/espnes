#pragma once
#include <stdint.h>

void display_init(void);
void display_clear_black(void);
void display_push_frame(int x, int y, int w, int h, const uint16_t *rgb565);
// level 0-10, linear, min ~2% max 100%
void display_set_backlight(int level);
// Cut backlight and hold GPIO low (call before light or deep sleep)
void display_sleep(void);
// Release GPIO hold and restore backlight (call after light sleep returns)
void display_wake(int level);
