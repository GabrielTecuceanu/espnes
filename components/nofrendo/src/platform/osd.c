/* osd.c — ESP32-S3 platform for NOFRENDO */
#include <stdio.h>
#include <string.h>
#include <noftypes.h>
#include <nofconfig.h>
#include <log.h>
#include <osd.h>
#include <nofrendo.h>
#include <bitmap.h>
#include <event.h>
#include <nesinput.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "display.h"
#include "audio_out.h"
#include "sd.h"

// ── Pins (active-low, internal pull-up) ───────────────────────────────────
#define BTN_UP     47
#define BTN_DOWN   39
#define BTN_LEFT   21
#define BTN_RIGHT  38
#define BTN_A      42
#define BTN_B      41
#define BTN_SELECT  1
#define BTN_START   2

#define NES_W  256
#define NES_H  240
#define SAMPLE_RATE 22050
#define FRAG_SIZE   128

char configfilename[] = "na";

// ── Audio ──────────────────────────────────────────────────────────────────
static void (*audio_cb)(void *buf, int len) = NULL;

void osd_setsound(void (*playfunc)(void *buf, int len)) { audio_cb = playfunc; }

void osd_getsoundinfo(sndinfo_t *info) {
    info->sample_rate = SAMPLE_RATE;
    info->bps         = 16;
}

// ── Video ──────────────────────────────────────────────────────────────────
static uint16_t  palette[256];           // NES palette → RGB565 (byte-swapped)
static uint8_t   pixel_buf[NES_W * NES_H]; // 8-bit palette-index framebuffer
static uint16_t  row_rgb[NES_W];           // one converted row
static bitmap_t *primary_bmp = NULL;       // allocated once in init, reused every frame

static int  vid_init_drv(int w, int h)  { (void)w; (void)h; return 0; }
static void vid_shutdown_drv(void)       {}
static int  vid_set_mode(int w, int h)   { (void)w; (void)h; return 0; }

static void vid_set_palette(rgb_t *pal) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = ((pal[i].r >> 3) << 11)
                   | ((pal[i].g >> 2) <<  5)
                   |  (pal[i].b >> 3);
        palette[i] = (c >> 8) | (c << 8); // byte-swap for ST7789V
    }
}

static void vid_clear(uint8 color) {
    memset(pixel_buf, color, sizeof(pixel_buf));
}

static bitmap_t *vid_lock_write(void) { return primary_bmp; }

// free_write not used in custom_blit path; keep as no-op to avoid double-free
static void vid_free_write(int num_dirties, rect_t *dirty_rects) {
    (void)num_dirties; (void)dirty_rects;
}

static void vid_custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects) {
    (void)num_dirties; (void)dirty_rects;

    // Convert palette indices → RGB565 row by row, push to display
    // Centered: 32px black border left/right (256 in 320)
    for (int y = 0; y < NES_H; y++) {
        const uint8_t *src = bmp->line[y];
        for (int x = 0; x < NES_W; x++)
            row_rgb[x] = palette[src[x]];
        display_push_frame(32, y, NES_W, 1, row_rgb);
    }

    // Drain audio for this frame
    if (audio_cb) {
        static int16_t audio_buf[FRAG_SIZE * 2];
        int left = SAMPLE_RATE / 60;
        while (left > 0) {
            int n = left < FRAG_SIZE ? left : FRAG_SIZE;
            audio_cb(audio_buf, n * 2); // NOFRENDO gives bytes
            audio_write(audio_buf, n);
            left -= n;
        }
    } else {
        audio_silence();
    }
}

static viddriver_t esp32_driver = {
    "ESP32-S3 ST7789V",
    vid_init_drv,
    vid_shutdown_drv,
    vid_set_mode,
    vid_set_palette,
    vid_clear,
    vid_lock_write,
    vid_free_write,
    vid_custom_blit,
    false
};

void osd_getvideoinfo(vidinfo_t *info) {
    info->default_width  = NES_W;
    info->default_height = NES_H;
    info->driver         = &esp32_driver;
}

// ── Timer ──────────────────────────────────────────────────────────────────
static TimerHandle_t nes_timer;

int osd_installtimer(int frequency, void *func, int funcsize,
                     void *counter, int countersize) {
    (void)funcsize; (void)counter; (void)countersize;
    nes_timer = xTimerCreate("nes", configTICK_RATE_HZ / frequency,
                             pdTRUE, NULL, (TimerCallbackFunction_t)func);
    xTimerStart(nes_timer, 0);
    return 0;
}

// ── Input ──────────────────────────────────────────────────────────────────
static void input_init(void) {
    const int pins[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
                        BTN_A, BTN_B, BTN_SELECT, BTN_START};
    uint64_t mask = 0;
    for (int i = 0; i < 8; i++) mask |= 1ULL << pins[i];
    gpio_config_t gc = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&gc);
}

static inline int btn(int gpio) { return gpio_get_level(gpio) == 0; }

void osd_getinput(void) {
    static int prev = 0;
    struct { int gpio; int ev_make; int ev_break; } map[] = {
        {BTN_A,      event_joypad1_a,      event_joypad1_a},
        {BTN_B,      event_joypad1_b,      event_joypad1_b},
        {BTN_SELECT, event_joypad1_select, event_joypad1_select},
        {BTN_START,  event_joypad1_start,  event_joypad1_start},
        {BTN_UP,     event_joypad1_up,     event_joypad1_up},
        {BTN_DOWN,   event_joypad1_down,   event_joypad1_down},
        {BTN_LEFT,   event_joypad1_left,   event_joypad1_left},
        {BTN_RIGHT,  event_joypad1_right,  event_joypad1_right},
    };
    int cur = 0;
    for (int i = 0; i < 8; i++)
        if (btn(map[i].gpio)) cur |= 1 << i;
    int changed = cur ^ prev;
    for (int i = 0; i < 8; i++) {
        if (changed & (1 << i)) {
            event_t evh = event_get(map[i].ev_make);
            if (evh)
                evh((cur & (1 << i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
        }
    }
    prev = cur;
}

void osd_getmouse(int *x, int *y, int *button) {
    (void)x; (void)y; (void)button;
}

// ── Init / shutdown ────────────────────────────────────────────────────────
static int logprint(const char *s) { return printf("%s", s); }

int osd_init(void) {
    log_chain_logfunc(logprint);
    display_init();
    audio_init(SAMPLE_RATE);
    input_init();
    // Allocate persistent framebuffer bitmap (reused every frame)
    primary_bmp = bmp_createhw(pixel_buf, NES_W, NES_H, NES_W);
    return primary_bmp ? 0 : -1;
}

void osd_shutdown(void) { audio_cb = NULL; }

// ── ROM data ───────────────────────────────────────────────────────────────
static uint8_t *rom_data = NULL;
static size_t   rom_size = 0;

void osd_setromdata(uint8_t *data, size_t size) { rom_data = data; rom_size = size; }
char *osd_getromdata(void) { return (char *)rom_data; }

int osd_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    config.filename = configfilename;

    extern const char *nes_rom_path;
    const char *path = nes_rom_path ? nes_rom_path : "/sdcard/rom.nes";

    if (sd_init() == 0) {
        size_t size = 0;
        uint8_t *data = sd_load_rom(path, &size);
        if (data) osd_setromdata(data, size);
    }

    return main_loop(path, system_autodetect);
}

// ── Filename helpers (no-op on ESP32) ─────────────────────────────────────
void osd_fullname(char *fullname, const char *shortname) {
    strncpy(fullname, shortname, PATH_MAX);
}
char *osd_newextension(char *string, char *ext) { (void)ext; return string; }
int   osd_makesnapname(char *filename, int len)  { (void)filename; (void)len; return -1; }
