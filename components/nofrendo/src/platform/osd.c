/* osd.c */
#include <bitmap.h>
#include <event.h>
#include <log.h>
#include <nesinput.h>
#include <nofconfig.h>
#include <nofrendo.h>
#include <noftypes.h>
#include <osd.h>
#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "nes.h"
#include "nes_rom.h"
#include "nesstate.h"
#include "sd.h"

#define BTN_UP 47
#define BTN_DOWN 39
#define BTN_LEFT 21
#define BTN_RIGHT 38
#define BTN_A 42
#define BTN_B 41
#define BTN_SELECT 1
#define BTN_START 2

#define NES_W 256
#define NES_H 240
#define SAMPLE_RATE 22050
#define FRAG_SIZE 128

char configfilename[] = "na";

static void (*volatile audio_cb)(void* buf, int len) = NULL;

static TaskHandle_t audio_task_handle = NULL;

static void mem_report(void) {
    multi_heap_info_t info;

    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    printf("\n=== Memory Report ===\n");
    printf("Internal SRAM:\n");
    printf("  free:        %u B (%u KB)\n",
           (unsigned)info.total_free_bytes, (unsigned)(info.total_free_bytes / 1024));
    printf("  allocated:   %u B (%u KB)\n",
           (unsigned)info.total_allocated_bytes, (unsigned)(info.total_allocated_bytes / 1024));
    printf("  largest blk: %u B (%u KB)\n",
           (unsigned)info.largest_free_block, (unsigned)(info.largest_free_block / 1024));

    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    printf("PSRAM:\n");
    printf("  free:        %u B (%u KB)\n",
           (unsigned)info.total_free_bytes, (unsigned)(info.total_free_bytes / 1024));
    printf("  allocated:   %u B (%u KB)\n",
           (unsigned)info.total_allocated_bytes, (unsigned)(info.total_allocated_bytes / 1024));
    printf("  largest blk: %u B (%u KB)\n",
           (unsigned)info.largest_free_block, (unsigned)(info.largest_free_block / 1024));

    printf("Stack high-water marks:\n");
    printf("  nes_main (core 0): %u B free\n",
           (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    if (audio_task_handle)
        printf("  nes_audio (core 1): %u B free\n",
               (unsigned)(uxTaskGetStackHighWaterMark(audio_task_handle) * sizeof(StackType_t)));
    printf("=====================\n\n");
}

void osd_setsound(void (*playfunc)(void* buf, int len)) {
    audio_cb = playfunc;
}

void osd_getsoundinfo(sndinfo_t* info) {
    info->sample_rate = SAMPLE_RATE;
    info->bps = 16;
}

static volatile int sw_vol = 10;
static volatile int64_t vol_show_until = 0;

static volatile int sw_bl = 10;
static volatile int64_t bl_show_until = 0;

#define SIDE_W 32

#define VOL_X 288
#define BL_X 0

#define SIDE_SEG 21
#define SIDE_DIG 4

static const uint8_t digit3x5[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
    {0x7, 0x1, 0x3, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1}, {0x7, 0x5, 0x7, 0x5, 0x7},
    {0x7, 0x5, 0x7, 0x1, 0x7},
};

static void draw_digit(uint16_t* band, int x0, int y0, int d) {
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++) {
            uint16_t c = (digit3x5[d][row] & (0x4 >> col)) ? 0xFFFFu : 0x0000u;
            for (int sy = 0; sy < SIDE_DIG; sy++)
                for (int sx = 0; sx < SIDE_DIG; sx++)
                    band[(y0 + row * SIDE_DIG + sy) * SIDE_W + x0 + col * SIDE_DIG + sx] = c;
        }
}

static uint16_t side_band[SIDE_W * 240];

static void draw_side_indicator(int x_start, int level, uint16_t bar_color) {
    memset(side_band, 0, sizeof(side_band));

    if (level >= 0) {
        int dw = 3 * SIDE_DIG;
        if (level < 10) {
            draw_digit(side_band, (SIDE_W - dw) / 2, 5, level);
        } else {
            int x0 = (SIDE_W - (2 * dw + 2)) / 2;
            draw_digit(side_band, x0, 5, 1);
            draw_digit(side_band, x0 + dw + 2, 5, 0);
        }
        for (int seg = 0; seg < level; seg++) {
            int y_bot = 239 - seg * SIDE_SEG;
            int y_top = y_bot - (SIDE_SEG - 3);
            for (int y = y_top; y <= y_bot; y++)
                for (int x = 3; x < SIDE_W - 3; x++) side_band[y * SIDE_W + x] = bar_color;
        }
    }

    display_push_frame(x_start, 0, SIDE_W, 160, side_band);
    display_push_frame(x_start, 160, SIDE_W, 80, side_band + 160 * SIDE_W);
}

static void draw_vol_indicator(int level) {
    draw_side_indicator(VOL_X, level, 0xE007u);
}

static void draw_bl_indicator(int level) {
    draw_side_indicator(BL_X, level, 0xE0FFu);
}

static uint16_t palette[256];
static uint8_t pixel_buf[NES_W * NES_H];
static uint16_t row_rgb[NES_W];
static bitmap_t* primary_bmp = NULL;

static int vid_init_drv(int w, int h) {
    (void)w;
    (void)h;
    return 0;
}

static void vid_shutdown_drv(void) {
}

static int vid_set_mode(int w, int h) {
    (void)w;
    (void)h;
    return 0;
}

static void vid_set_palette(rgb_t* pal) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = ((pal[i].r >> 3) << 11) | ((pal[i].g >> 2) << 5) | (pal[i].b >> 3);
        palette[i] = (c >> 8) | (c << 8);
    }
}

static void vid_clear(uint8 color) {
    memset(pixel_buf, color, sizeof(pixel_buf));
}

static bitmap_t* vid_lock_write(void) {
    return primary_bmp;
}

static void vid_free_write(int num_dirties, rect_t* dirty_rects) {
    (void)num_dirties;
    (void)dirty_rects;
}

static void vid_custom_blit(bitmap_t* bmp, int num_dirties, rect_t* dirty_rects) {
    (void)num_dirties;
    (void)dirty_rects;

    int h = bmp->height;
    int y_off = (240 - h) / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = bmp->line[y];
        for (int x = 0; x < NES_W; x++) row_rgb[x] = palette[src[x]];
        display_push_frame(32, y_off + y, NES_W, 1, row_rgb);
    }

    static int save_ticker = 0;
    if (++save_ticker >= 1800) {
        save_ticker = 0;
        nes_t* nes = nes_getcontextptr();
        if (nes && nes->rominfo)
            rom_savesram(nes->rominfo);
    }

    int64_t now = esp_timer_get_time();

    static int vol_drawn = -1;
    int cur_vol = sw_vol;
    if (now < vol_show_until) {
        if (cur_vol != vol_drawn) {
            draw_vol_indicator(cur_vol);
            vol_drawn = cur_vol;
        }
    } else if (vol_drawn >= 0) {
        draw_vol_indicator(-1);
        vol_drawn = -1;
    }

    static int bl_drawn = -1;
    int cur_bl = sw_bl;
    if (now < bl_show_until) {
        if (cur_bl != bl_drawn) {
            draw_bl_indicator(cur_bl);
            bl_drawn = cur_bl;
        }
    } else if (bl_drawn >= 0) {
        draw_bl_indicator(-1);
        bl_drawn = -1;
    }
}

#define VOL_CH ADC_CHANNEL_6

static adc_oneshot_unit_handle_t vol_adc = NULL;

static void audio_task_fn(void* arg) {
    (void)arg;
    static int16_t buf[FRAG_SIZE];
    float vol = 1.0f;
    int tick = 0;

    for (;;) {
        void (*cb)(void*, int) = audio_cb;
        if (cb)
            cb(buf, FRAG_SIZE);
        else
            memset(buf, 0, sizeof(buf));

        if (++tick >= 8) {
            tick = 0;
            int raw = 4095;
            if (vol_adc)
                adc_oneshot_read(vol_adc, VOL_CH, &raw);
            float v = raw / 4095.0f;
            float sw_scale = (sw_vol >= 10) ? 1.3f : sw_vol / 10.0f;
            vol = v * v * sw_scale;
        }

        for (int i = 0; i < FRAG_SIZE; i++) buf[i] = (int16_t)(buf[i] * vol);

        audio_write(buf, FRAG_SIZE);
    }
}

static viddriver_t esp32_driver = {
    "ESP32-S3 ST7789V", vid_init_drv,   vid_shutdown_drv, vid_set_mode,    vid_set_palette,
    vid_clear,          vid_lock_write, vid_free_write,   vid_custom_blit, false};

void osd_getvideoinfo(vidinfo_t* info) {
    info->default_width = NES_W;
    info->default_height = NES_H;
    info->driver = &esp32_driver;
}

static char osd_sel_path[300] = SD_ROM_DIR "/rom.nes";

static esp_timer_handle_t nes_timer = NULL;
static void (*nes_tick_cb)(void) = NULL;
static void nes_timer_cb(void* arg) {
    (void)arg;
    if (nes_tick_cb)
        nes_tick_cb();
}

int osd_installtimer(int frequency, void* func, int funcsize, void* counter, int countersize) {
    (void)funcsize;
    (void)counter;
    (void)countersize;
    nes_tick_cb = (void (*)(void))func;
    if (nes_timer) {
        esp_timer_stop(nes_timer);
        esp_timer_delete(nes_timer);
        nes_timer = NULL;
    }
    const esp_timer_create_args_t args = {
        .callback = nes_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nes",
    };
    esp_timer_create(&args, &nes_timer);
    esp_timer_start_periodic(nes_timer, 1000000 / frequency);
    return 0;
}

static void input_init(void) {
    const int pins[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_SELECT, BTN_START};
    uint64_t mask = 0;
    for (int i = 0; i < 8; i++) mask |= 1ULL << pins[i];
    gpio_config_t gc = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&gc);
}

static inline int btn(int gpio) {
    return gpio_get_level(gpio) == 0;
}

#define SETTINGS_PATH "/sdcard/settings.cfg"

static void settings_load(void) {
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f)
        return;
    char line[32];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "vol=%d", &v) == 1 && v >= 0 && v <= 10)
            sw_vol = v;
        if (sscanf(line, "bl=%d", &v) == 1 && v >= 0 && v <= 10)
            sw_bl = v;
    }
    fclose(f);
    display_set_backlight(sw_bl);
}

static void settings_save(void) {
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "vol=%d\nbl=%d\n", (int)sw_vol, (int)sw_bl);
    fclose(f);
}

#define AUTO_SLEEP_US (3LL * 60 * 1000000)

static volatile bool osd_return_to_menu = false;

static void do_light_sleep(void) {
    settings_save();
    display_sleep();
    gpio_wakeup_enable(BTN_START, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    while (gpio_get_level(BTN_START) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    display_clear_black();
    display_wake(sw_bl);
}

static void handle_auto_sleep(int cur) {
    static int64_t last_input_time = 0;
    int64_t now = esp_timer_get_time();
    if (cur)
        last_input_time = now;
    if (last_input_time == 0)
        last_input_time = now;
    if (now - last_input_time < AUTO_SLEEP_US)
        return;
    last_input_time = now;
    nes_t* nes = nes_getcontextptr();
    if (nes && nes->rominfo)
        rom_savesram(nes->rominfo);
    do_light_sleep();
    last_input_time = esp_timer_get_time();
}

static bool handle_pause_menu(int cur, int* suppress) {
    static int64_t select_a_since = 0;
    if (!((cur & (1 << 2)) && (cur & (1 << 0)))) {
        select_a_since = 0;
        return false;
    }
    *suppress |= (1 << 2) | (1 << 0);
    int64_t now = esp_timer_get_time();
    if (select_a_since == 0)
        select_a_since = now;
    if (now - select_a_since < 500000)
        return false;

    select_a_since = 0;
    void (*saved_cb)(void* buf, int len) = audio_cb;
    audio_cb = NULL;
    display_clear_black();
    int ticks_at_pause = nofrendo_ticks;

    pause_result_t pr;
    for (;;) {
        pr = menu_pause(osd_sel_path);
        if (pr.action == PAUSE_SAVE) {
            state_setslot(pr.slot);
            state_save();
        } else {
            break;
        }
    }
    if (pr.action == PAUSE_LOAD) {
        state_setslot(pr.slot);
        state_load();
    }
    switch (pr.action) {
        case PAUSE_ROM_MENU: {
            nes_t* nes = nes_getcontextptr();
            if (nes && nes->rominfo)
                rom_savesram(nes->rominfo);
            osd_return_to_menu = true;
            main_quit();
            return true;
        }
        case PAUSE_SLEEP: {
            nes_t* nes = nes_getcontextptr();
            if (nes && nes->rominfo)
                rom_savesram(nes->rominfo);
            do_light_sleep();
            break;
        }
        case PAUSE_MEM_REPORT:
            mem_report();
            break;
        default:
            break;
    }

    display_clear_black();
    audio_cb = saved_cb;
    nofrendo_ticks = ticks_at_pause + 1;
    return false;
}

static void handle_select_combos(int cur, int changed, int* suppress) {
    if (!(cur & (1 << 2)))
        return;

    int64_t now = esp_timer_get_time();

    if ((changed & (1 << 4)) && (cur & (1 << 4))) {
        if (sw_vol < 10)
            sw_vol++;
        vol_show_until = now + 3000000LL;
        *suppress |= (1 << 4);
        settings_save();
    }

    if ((changed & (1 << 5)) && (cur & (1 << 5))) {
        if (sw_vol > 0)
            sw_vol--;
        vol_show_until = now + 3000000LL;
        *suppress |= (1 << 5);
        settings_save();
    }

    if ((changed & (1 << 7)) && (cur & (1 << 7))) {
        if (sw_bl < 10)
            sw_bl++;
        display_set_backlight(sw_bl);
        bl_show_until = now + 3000000LL;
        *suppress |= (1 << 7);
        settings_save();
    }

    if ((changed & (1 << 6)) && (cur & (1 << 6))) {
        if (sw_bl > 0)
            sw_bl--;
        display_set_backlight(sw_bl);
        bl_show_until = now + 3000000LL;
        *suppress |= (1 << 6);
        settings_save();
    }
}

void osd_getinput(void) {
    static int prev = 0;
    struct {
        int gpio;
        int ev;
    } map[] = {
        {BTN_A, event_joypad1_a},           {BTN_B, event_joypad1_b},
        {BTN_SELECT, event_joypad1_select}, {BTN_START, event_joypad1_start},
        {BTN_UP, event_joypad1_up},         {BTN_DOWN, event_joypad1_down},
        {BTN_LEFT, event_joypad1_left},     {BTN_RIGHT, event_joypad1_right},
    };

    int cur = 0;
    for (int i = 0; i < 8; i++)
        if (btn(map[i].gpio))
            cur |= 1 << i;
    int changed = cur ^ prev;
    int suppress = 0;

    handle_auto_sleep(cur);
    if (handle_pause_menu(cur, &suppress))
        return;
    handle_select_combos(cur, changed, &suppress);

    for (int i = 0; i < 8; i++) {
        if (suppress & (1 << i))
            continue;
        if (changed & (1 << i)) {
            event_t evh = event_get(map[i].ev);
            if (evh)
                evh((cur & (1 << i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
        }
    }
    prev = cur;
}

void osd_getmouse(int* x, int* y, int* button) {
    (void)x;
    (void)y;
    (void)button;
}

static int logprint(const char* s) {
    return printf("%s", s);
}

static uint8_t* rom_data = NULL;
static size_t rom_size = 0;

void osd_setromdata(uint8_t* data, size_t size) {
    rom_data = data;
    rom_size = size;
}
char* osd_getromdata(void) {
    return (char*)rom_data;
}

static void osd_select_rom(void) {
    static bool sd_mounted = false;

    if (!sd_mounted) {
        sd_mounted = (sd_init() == 0);
        if (sd_mounted)
            settings_load();
    }

    if (!sd_mounted)
        return;

    static char rom_names[SD_MAX_ROMS][SD_NAME_LEN];
    int count = sd_list_roms(rom_names, SD_MAX_ROMS);
    int sel = menu_select((const char (*)[SD_NAME_LEN])rom_names, count);
    display_clear_black();

    if (sel >= 0)
        snprintf(osd_sel_path, sizeof(osd_sel_path), SD_ROM_DIR "/%s", rom_names[sel]);

    if (rom_data) {
        free(rom_data);
        rom_data = NULL;
        rom_size = 0;
    }

    size_t size = 0;
    uint8_t* data = sd_load_rom(osd_sel_path, &size);

    if (data)
        osd_setromdata(data, size);
    else
        printf("osd: ROM load failed: %s\n", osd_sel_path);
}

int osd_init(void) {
    return 0;
}

void osd_shutdown(void) {
    audio_cb = NULL;
}

int osd_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    config.filename = configfilename;

    log_chain_logfunc(logprint);
    display_init();
    audio_init(SAMPLE_RATE);
    input_init();

    adc_oneshot_unit_init_cfg_t adc_cfg = {.unit_id = ADC_UNIT_1};
    adc_oneshot_new_unit(&adc_cfg, &vol_adc);
    adc_oneshot_chan_cfg_t ch_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(vol_adc, VOL_CH, &ch_cfg);

    primary_bmp = bmp_createhw(pixel_buf, NES_W, NES_H, NES_W);
    if (!primary_bmp)
        return -1;

    xTaskCreatePinnedToCore(audio_task_fn, "nes_audio", 4096, NULL, configMAX_PRIORITIES - 1,
                            &audio_task_handle, 1);

    osd_select_rom();
    for (;;) {
        if (!rom_data) {
            osd_select_rom();
            continue;
        }
        mem_report();
        main_loop(osd_sel_path, system_autodetect);
        bool came_from_game = osd_return_to_menu;
        osd_return_to_menu = false;
        osd_select_rom();
        if (came_from_game)
            main_reset();
    }
    return 0;
}

void osd_fullname(char* fullname, const char* shortname) {
    strncpy(fullname, shortname, PATH_MAX);
}

char* osd_newextension(char* string, char* ext) {
    char* slash = strrchr(string, '/');
    const char* base = slash ? slash + 1 : string;

    char stem[256];
    strncpy(stem, base, 255);
    stem[255] = '\0';

    char* dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';

    snprintf(string, PATH_MAX + 1, SD_SAVE_DIR "/%s%s", stem, ext);
    return string;
}

int osd_makesnapname(char* filename, int len) {
    (void)filename;
    (void)len;
    return -1;
}
