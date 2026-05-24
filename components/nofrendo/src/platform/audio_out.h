#pragma once

#include <stdint.h>

void audio_init(int sample_rate);

void audio_write(const int16_t* buf, int n_samples);
