#pragma once
#include "sd.h"

int menu_select(const char names[][SD_NAME_LEN], int count);

typedef enum {
    PAUSE_RESUME   = 0,
    PAUSE_SAVE     = 1,
    PAUSE_LOAD     = 2,
    PAUSE_ROM_MENU = 3,
    PAUSE_SLEEP    = 4,
} pause_action_t;

typedef struct {
    pause_action_t action;
    int slot;  /* 0-9, valid when action == PAUSE_SAVE or PAUSE_LOAD */
} pause_result_t;

pause_result_t menu_pause(const char *rom_path);
