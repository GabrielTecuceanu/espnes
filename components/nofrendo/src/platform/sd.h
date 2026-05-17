#pragma once
#include <stdint.h>
#include <stddef.h>

// Mount SD card on shared SPI2 bus (CS=17). Call after display_init().
int sd_init(void);

// Read a .nes file into heap (PSRAM if available), validate iNES magic.
// Returns pointer on success (caller owns it), NULL on error.
uint8_t *sd_load_rom(const char *path, size_t *out_size);
