#pragma once

#include <stddef.h>
#include <stdint.h>

#define SD_MAX_ROMS 200
#define SD_NAME_LEN 128

#define SD_ROM_DIR "/sdcard/ROMS"
#define SD_SAVE_DIR "/sdcard/saves"

int sd_init(void);

int sd_list_roms(char names[][SD_NAME_LEN], int max_count);

uint8_t* sd_load_rom(const char* path, size_t* out_size);
