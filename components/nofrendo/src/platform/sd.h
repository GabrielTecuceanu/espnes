#pragma once
#include <stdint.h>
#include <stddef.h>

#define SD_MAX_ROMS  64
#define SD_NAME_LEN  64

// Mount SD card on shared SPI2 bus (CS=17). Call after display_init().
int sd_init(void);

// Scan /sdcard for .nes files. Returns count found; names[][] filled with bare filenames.
int sd_list_roms(char names[][SD_NAME_LEN], int max_count);

// Read a .nes file into heap (PSRAM if available), validate iNES magic.
// Returns pointer on success (caller owns it), NULL on error.
uint8_t *sd_load_rom(const char *path, size_t *out_size);
