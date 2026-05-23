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
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "display.h"
#include "audio_out.h"
#include "sd.h"
#include "menu.h"
#include "nes.h"
#include "nes_rom.h"

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

// ── Volume indicator ───────────────────────────────────────────────────────
static volatile int     sw_vol           = 10;  // 0-10 software level
static volatile int64_t vol_show_until   = 0;   // µs timestamp until indicator is shown

#define VOL_X    288   // right side band
#define VOL_W     32
#define VOL_SEG   21   // px per segment (19 fill + 2 gap)
#define VOL_DIG   4    // digit scale factor (3×5 → 12×20 px)

// 3×5 bitmap digits, MSB = left pixel
static const uint8_t digit3x5[10][5] = {
    {0x7,0x5,0x5,0x5,0x7},  // 0
    {0x2,0x6,0x2,0x2,0x7},  // 1
    {0x7,0x1,0x7,0x4,0x7},  // 2
    {0x7,0x1,0x3,0x1,0x7},  // 3
    {0x5,0x5,0x7,0x1,0x1},  // 4
    {0x7,0x4,0x7,0x1,0x7},  // 5
    {0x7,0x4,0x7,0x5,0x7},  // 6
    {0x7,0x1,0x1,0x1,0x1},  // 7
    {0x7,0x5,0x7,0x5,0x7},  // 8
    {0x7,0x5,0x7,0x1,0x7},  // 9
};

static void draw_digit(uint16_t *band, int x0, int y0, int d) {
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++) {
            uint16_t c = (digit3x5[d][row] & (0x4 >> col)) ? 0xFFFFu : 0x0000u;
            for (int sy = 0; sy < VOL_DIG; sy++)
                for (int sx = 0; sx < VOL_DIG; sx++)
                    band[(y0 + row*VOL_DIG + sy)*VOL_W + x0 + col*VOL_DIG + sx] = c;
        }
}

static void draw_vol_indicator(int level) {
    // level 0-10; -1 = clear to black
    static uint16_t band[VOL_W * 240];  // 15 KB in BSS — zero-init on first call
    memset(band, 0, sizeof(band));

    if (level >= 0) {
        // ── Digit(s) at top (y=5, height=20px) ───────────────────────────
        int dw = 3 * VOL_DIG;  // 12px per digit
        if (level < 10) {
            draw_digit(band, (VOL_W - dw) / 2, 5, level);
        } else {
            int x0 = (VOL_W - (2*dw + 2)) / 2;
            draw_digit(band, x0,        5, 1);
            draw_digit(band, x0+dw+2,   5, 0);
        }

        // ── Bar segments from bottom (y=239 upward) ───────────────────────
        // green segments: 0xE007 (bswap of 0x07E0)
        for (int seg = 0; seg < level; seg++) {
            int y_bot = 239 - seg * VOL_SEG;
            int y_top = y_bot - (VOL_SEG - 3);  // 3px gap per segment
            for (int y = y_top; y <= y_bot; y++)
                for (int x = 3; x < VOL_W - 3; x++)
                    band[y * VOL_W + x] = 0xE007u;
        }
    }

    // Push in two SPI-safe chunks (32×160 and 32×80 = 10240 and 5120 bytes)
    display_push_frame(VOL_X,   0, VOL_W, 160, band);
    display_push_frame(VOL_X, 160, VOL_W,  80, band + 160*VOL_W);
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

    int h = bmp->height;
    int y_off = (240 - h) / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = bmp->line[y];
        for (int x = 0; x < NES_W; x++)
            row_rgb[x] = palette[src[x]];
        display_push_frame(32, y_off + y, NES_W, 1, row_rgb);
    }

    // Auto-save SRAM every 30 seconds (1800 frames @ 60Hz)
    static int save_ticker = 0;
    if (++save_ticker >= 1800) {
        save_ticker = 0;
        nes_t *nes = nes_getcontextptr();
        if (nes && nes->rominfo)
            rom_savesram(nes->rominfo);
    }

    // Volume indicator: show while timer active, clear once after expiry
    static int vol_drawn = -1;
    int cur_vol = sw_vol;
    if (esp_timer_get_time() < vol_show_until) {
        if (cur_vol != vol_drawn) {
            draw_vol_indicator(cur_vol);
            vol_drawn = cur_vol;
        }
    } else if (vol_drawn >= 0) {
        draw_vol_indicator(-1);  // clear
        vol_drawn = -1;
    }
}

// ── Audio task (Core 1) ────────────────────────────────────────────────────
#define VOL_CH  ADC_CHANNEL_6   // GPIO7 → ADC1 channel 6

static adc_oneshot_unit_handle_t vol_adc = NULL;

static void audio_task_fn(void *arg) {
    (void)arg;
    static int16_t buf[FRAG_SIZE];
    float vol = 1.0f;
    int   tick = 0;

    for (;;) {
        if (audio_cb)
            audio_cb(buf, FRAG_SIZE);
        else
            memset(buf, 0, sizeof(buf));

        // Re-read ADC every ~50 ms (8 chunks × 5.8 ms each)
        if (++tick >= 8) {
            tick = 0;
            int raw = 4095;
            if (vol_adc)
                adc_oneshot_read(vol_adc, VOL_CH, &raw);
            float v = raw / 4095.0f;
            vol = v * v * (sw_vol / 10.0f);  // ADC quadratic × software level
        }

        for (int i = 0; i < FRAG_SIZE; i++)
            buf[i] = (int16_t)(buf[i] * vol);

        audio_write(buf, FRAG_SIZE);
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
static esp_timer_handle_t nes_timer;
static void (*nes_tick_cb)(void) = NULL;
static void nes_timer_cb(void *arg) { (void)arg; if (nes_tick_cb) nes_tick_cb(); }

int osd_installtimer(int frequency, void *func, int funcsize,
                     void *counter, int countersize) {
    (void)funcsize; (void)counter; (void)countersize;
    nes_tick_cb = (void(*)(void))func;
    const esp_timer_create_args_t args = {
        .callback = nes_timer_cb,
        .arg      = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "nes",
    };
    esp_timer_create(&args, &nes_timer);
    esp_timer_start_periodic(nes_timer, 1000000 / frequency); // microseconds → exact 60 Hz
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

    // SELECT (bit 2) + UP (bit 4) / DOWN (bit 5) = software volume control
    // Suppress UP/DOWN from reaching the NES while this combo is active.
    int suppress = 0;
    if (cur & (1 << 2)) {
        if ((changed & (1 << 4)) && (cur & (1 << 4))) {
            if (sw_vol < 10) sw_vol++;
            vol_show_until = esp_timer_get_time() + 3000000LL;
            suppress |= (1 << 4);
        }
        if ((changed & (1 << 5)) && (cur & (1 << 5))) {
            if (sw_vol > 0) sw_vol--;
            vol_show_until = esp_timer_get_time() + 3000000LL;
            suppress |= (1 << 5);
        }
    }

    for (int i = 0; i < 8; i++) {
        if (suppress & (1 << i)) continue;
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

// ── ROM data ───────────────────────────────────────────────────────────────
static uint8_t *rom_data = NULL;
static size_t   rom_size = 0;
static char     osd_sel_path[128] = "/sdcard/rom.nes";

void osd_setromdata(uint8_t *data, size_t size) { rom_data = data; rom_size = size; }
char *osd_getromdata(void) { return (char *)rom_data; }

int osd_init(void) {
    log_chain_logfunc(logprint);
    display_init();           // initializes SPI2 bus — must come before sd_init
    audio_init(SAMPLE_RATE);
    input_init();

    if (sd_init() == 0) {
        static char rom_names[SD_MAX_ROMS][SD_NAME_LEN];
        int count = sd_list_roms(rom_names, SD_MAX_ROMS);
        int sel = menu_select((const char (*)[SD_NAME_LEN])rom_names, count);
        display_clear_black();  // erase menu artifacts from side bands
        if (sel >= 0)
            snprintf(osd_sel_path, sizeof(osd_sel_path), "/sdcard/%s", rom_names[sel]);

        size_t size = 0;
        uint8_t *data = sd_load_rom(osd_sel_path, &size);
        if (data) osd_setromdata(data, size);
    }

    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &vol_adc);
    adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(vol_adc, VOL_CH, &ch_cfg);

    primary_bmp = bmp_createhw(pixel_buf, NES_W, NES_H, NES_W);
    if (!primary_bmp) return -1;

    xTaskCreatePinnedToCore(audio_task_fn, "nes_audio", 4096, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);
    return 0;
}

void osd_shutdown(void) { audio_cb = NULL; }

int osd_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    config.filename = configfilename;
    return main_loop(osd_sel_path, system_autodetect);
}

// ── Filename helpers (no-op on ESP32) ─────────────────────────────────────
void osd_fullname(char *fullname, const char *shortname) {
    strncpy(fullname, shortname, PATH_MAX);
}
char *osd_newextension(char *string, char *ext) {
    char *dot = strrchr(string, '.');
    if (dot) strcpy(dot, ext);
    return string;
}
int   osd_makesnapname(char *filename, int len)  { (void)filename; (void)len; return -1; }
