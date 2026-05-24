# espNES

NES emulator on ESP32-S3 DevKitC-1 N16R8.

---

## SD Card layout

```
/sdcard/
  ROMS/        <- put .nes files here
  saves/       <- save states (auto-created)
  settings.cfg <- volume & backlight (auto-created)
```

---

## Controls

| Shortcut            | Action            |
| ------------------- | ----------------- |
| SELECT + UP/DOWN    | Volume control    |
| SELECT + LEFT/RIGHT | Backlight control |
| SELECT + A (hold)   | Go to pause menu  |

Auto-sleep triggers after 3 minutes of no input.

---

## Project structure

```
main/
  main.c                            <- entry point

components/nofrendo/src/platform/   <- all hardware drivers (written by us)
  osd.c                             <- main integration: input, video, timer, sleep
  display.c / .h                    <- ST7789V SPI display driver
  audio_out.c / .h                  <- I2S audio (MAX98357A)
  sd.c / .h                         <- SD card mount, ROM listing, ROM loading
  menu.c / .h                       <- LVGL ROM selection + pause menu

components/nofrendo/                <- NOFRENDO NES emulator (MIT, unmodified except nes.c)
```

---

## Build & flash

```bash
idf.py build
idf.py flash monitor
```
