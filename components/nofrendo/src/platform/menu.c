/* menu.c - LVGL ROM selection menu for espNES */
#include "menu.h"
#include "display.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

#define TAG "menu"

#define BTN_UP    47
#define BTN_DOWN  39
#define BTN_A     42
#define BTN_START  2

#define DISP_W   320
#define DISP_H   240
#define BUF_ROWS  10
#define BUF_SIZE (DISP_W * BUF_ROWS)

/* Two separate buffers: LVGL renders into lvgl_buf, flush_cb byte-swaps into
   spi_buf so LVGL can immediately reuse lvgl_buf while SPI DMA is still running */
static lv_color_t lvgl_buf[BUF_SIZE];
static uint16_t   spi_buf[BUF_SIZE];

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_disp_t *disp;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    int n = w * h;
    uint16_t *src = (uint16_t *)pixels;
    for (int i = 0; i < n; i++)
        spi_buf[i] = (src[i] >> 8) | (src[i] << 8);

    display_push_frame(area->x1, area->y1, w, h, spi_buf);
    lv_disp_flush_ready(drv);
}

static void lvgl_init(void)
{
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISP_W;
    disp_drv.ver_res  = DISP_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp = lv_disp_drv_register(&disp_drv);
}

static void lvgl_tick(void)
{
    static uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_tick_inc(now - last_ms);
    last_ms = now;
    lv_timer_handler();
}

int menu_select(const char names[][SD_NAME_LEN], int count)
{
    if (count == 0) return -1;
    if (count == 1) {
        ESP_LOGI(TAG, "Auto-selecting: %s", names[0]);
        return 0;
    }

    lvgl_init();

    /* ── Screen: navy background ───────────────────────────────────────── */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ── Title bar (top 28px) ──────────────────────────────────────────── */
    lv_obj_t *title_bar = lv_obj_create(scr);
    lv_obj_set_size(title_bar, DISP_W, 28);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_make(30, 10, 80), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, "espNES  -  SELECT ROM");
    lv_obj_set_style_text_color(title, lv_color_make(255, 210, 0), 0);
    lv_obj_center(title);

    /* ── ROM list (middle) ─────────────────────────────────────────────── */
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, DISP_W, DISP_H - 28 - 24);
    lv_obj_set_pos(list, 0, 28);
    lv_obj_set_style_bg_color(list, lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_style_pad_left(list, 0, 0);
    lv_obj_set_style_pad_right(list, 0, 0);
    lv_obj_set_style_pad_top(list, 2, 0);

    lv_obj_t *btns[SD_MAX_ROMS];
    for (int i = 0; i < count; i++) {
        btns[i] = lv_list_add_btn(list, NULL, names[i]);
        lv_obj_set_style_bg_color(btns[i], lv_color_make(15, 15, 40), 0);
        lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btns[i], lv_color_make(200, 200, 200), 0);
        lv_obj_set_style_border_width(btns[i], 0, 0);
        lv_obj_clear_flag(btns[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* ── Hint bar (bottom 24px) ────────────────────────────────────────── */
    lv_obj_t *hint_bar = lv_obj_create(scr);
    lv_obj_set_size(hint_bar, DISP_W, 24);
    lv_obj_set_pos(hint_bar, 0, DISP_H - 24);
    lv_obj_set_style_bg_color(hint_bar, lv_color_make(30, 10, 80), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_radius(hint_bar, 0, 0);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_clear_flag(hint_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(hint_bar);
    lv_label_set_text(hint, "UP / DOWN: navigate      A / START: select");
    lv_obj_set_style_text_color(hint, lv_color_make(160, 160, 160), 0);
    lv_obj_center(hint);

    /* ── Highlight helper ──────────────────────────────────────────────── */
    int sel = 0;
#define HIGHLIGHT(i, on) do { \
    lv_obj_set_style_bg_color(btns[i], \
        (on) ? lv_color_make(0, 90, 210)   : lv_color_make(15, 15, 40), 0); \
    lv_obj_set_style_text_color(btns[i], \
        (on) ? lv_color_make(255, 230, 0)  : lv_color_make(200, 200, 200), 0); \
} while (0)

    HIGHLIGHT(sel, true);

    /* Force full initial render */
    lv_obj_invalidate(scr);
    for (int i = 0; i < 5; i++) lvgl_tick();

    /* ── Button polling loop ───────────────────────────────────────────── */
    /* Seed prev with currently-held buttons so we don't treat a held A/START
       (from the SELECT+A return shortcut) as a fresh press on menu open. */
    int prev = 0;
    if (gpio_get_level(BTN_UP)    == 0) prev |= 1;
    if (gpio_get_level(BTN_DOWN)  == 0) prev |= 2;
    if (gpio_get_level(BTN_A)     == 0) prev |= 4;
    if (gpio_get_level(BTN_START) == 0) prev |= 8;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();

        int cur = 0;
        if (gpio_get_level(BTN_UP)    == 0) cur |= 1;
        if (gpio_get_level(BTN_DOWN)  == 0) cur |= 2;
        if (gpio_get_level(BTN_A)     == 0) cur |= 4;
        if (gpio_get_level(BTN_START) == 0) cur |= 8;

        int pressed = cur & ~prev;
        prev = cur;

        if ((pressed & 1) && sel > 0) {
            HIGHLIGHT(sel, false);
            sel--;
            HIGHLIGHT(sel, true);
            lv_obj_scroll_to_view(btns[sel], LV_ANIM_ON);
        }
        if ((pressed & 2) && sel < count - 1) {
            HIGHLIGHT(sel, false);
            sel++;
            HIGHLIGHT(sel, true);
            lv_obj_scroll_to_view(btns[sel], LV_ANIM_ON);
        }
        if (pressed & (4 | 8)) {
            ESP_LOGI(TAG, "Selected: %s", names[sel]);
            break;
        }
    }

#undef HIGHLIGHT

    lv_obj_clean(scr);
    lv_deinit();
    return sel;
}
