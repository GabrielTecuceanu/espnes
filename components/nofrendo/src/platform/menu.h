#pragma once
#include "sd.h"

// Show ROM selection list. Returns selected index, or 0 if count==1 (auto), -1 if count==0.
int menu_select(const char names[][SD_NAME_LEN], int count);
