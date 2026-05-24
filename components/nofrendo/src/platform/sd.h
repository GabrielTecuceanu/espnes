#pragma once
#include <stddef.h>
#include <stdint.h>

#define SD_MAX_ROMS 200
#define SD_NAME_LEN 128

#define SD_ROM_DIR "/sdcard/ROMS"
#define SD_SAVE_DIR "/sdcard/saves"

// Mount SD card on shared SPI2 bus (CS=17). Call after display_init().
// Also creates SD_ROM_DIR and SD_SAVE_DIR if they don't exist yet.
int sd_init(void);

// Scan SD_ROM_DIR for .nes files. Returns count; names[][] filled with bare filenames.
int sd_list_roms(char names[][SD_NAME_LEN], int max_count);

// Read a .nes file into heap (PSRAM if available), validate iNES magic.
// Returns pointer on success (caller owns it), NULL on error.
uint8_t* sd_load_rom(const char* path, size_t* out_size);
