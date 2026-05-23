#pragma once
#include <stdint.h>

void display_init(void);
void display_clear_black(void);
void display_push_frame(int x, int y, int w, int h, const uint16_t *rgb565);
