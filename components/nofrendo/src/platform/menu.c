/* menu.c - LVGL ROM selection menu + in-game pause menu for espNES */
#include "menu.h"
#include "display.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#define TAG "menu"

#define BTN_UP    47
#define BTN_DOWN  39
#define BTN_A     42
#define BTN_B     41
#define BTN_START  2

#define DISP_W   320
#define DISP_H   240
#define BUF_ROWS  10
#define BUF_SIZE (DISP_W * BUF_ROWS)
#define MENU_ROW_H 22
#define MENU_VISIBLE_ROWS ((DISP_H - 28 - 24 - 4) / MENU_ROW_H)
#define MENU_REPEAT_DELAY_MS 300
#define MENU_REPEAT_RATE_MS   70

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

    /* ── ROM list (middle): fixed visible rows, labels updated while moving ─ */
    lv_obj_t *rows[MENU_VISIBLE_ROWS];
    lv_obj_t *labels[MENU_VISIBLE_ROWS];
    for (int i = 0; i < MENU_VISIBLE_ROWS; i++) {
        rows[i] = lv_obj_create(scr);
        lv_obj_set_size(rows[i], DISP_W, MENU_ROW_H);
        lv_obj_set_pos(rows[i], 0, 30 + i * MENU_ROW_H);
        lv_obj_set_style_bg_color(rows[i], lv_color_make(15, 15, 40), 0);
        lv_obj_set_style_bg_opa(rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rows[i], 0, 0);
        lv_obj_set_style_radius(rows[i], 0, 0);
        lv_obj_set_style_pad_all(rows[i], 0, 0);
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_SCROLLABLE);

        labels[i] = lv_label_create(rows[i]);
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(labels[i], DISP_W - 12);
        lv_obj_set_pos(labels[i], 6, 3);
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
    lv_label_set_text(hint, "UP/DOWN: move   A/START: select");
    lv_obj_set_style_text_color(hint, lv_color_make(160, 160, 160), 0);
    lv_obj_center(hint);

    /* ── Highlight/render helpers ──────────────────────────────────────── */
    int sel = 0;
#define RENDER_ROWS() do { \
    int top = sel - (MENU_VISIBLE_ROWS / 2); \
    if (top < 0) top = 0; \
    if (top > count - MENU_VISIBLE_ROWS) top = count - MENU_VISIBLE_ROWS; \
    if (top < 0) top = 0; \
    for (int i = 0; i < MENU_VISIBLE_ROWS; i++) { \
        int idx = top + i; \
        if (idx >= count) { \
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN); \
            continue; \
        } \
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN); \
        lv_label_set_text(labels[i], names[idx]); \
        lv_obj_set_style_bg_color(rows[i], \
            (idx == sel) ? lv_color_make(0, 90, 210) : lv_color_make(15, 15, 40), 0); \
        lv_obj_set_style_text_color(labels[i], \
            (idx == sel) ? lv_color_make(255, 230, 0) : lv_color_make(200, 200, 200), 0); \
    } \
} while (0)

    RENDER_ROWS();

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
    int64_t next_up_repeat = 0;
    int64_t next_down_repeat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();
        int64_t now = esp_timer_get_time() / 1000;

        int cur = 0;
        if (gpio_get_level(BTN_UP)    == 0) cur |= 1;
        if (gpio_get_level(BTN_DOWN)  == 0) cur |= 2;
        if (gpio_get_level(BTN_A)     == 0) cur |= 4;
        if (gpio_get_level(BTN_START) == 0) cur |= 8;

        int pressed = cur & ~prev;
        prev = cur;

        bool move_up = false;
        bool move_down = false;
        if (pressed & 1) {
            move_up = true;
            next_up_repeat = now + MENU_REPEAT_DELAY_MS;
        } else if ((cur & 1) && now >= next_up_repeat) {
            move_up = true;
            next_up_repeat = now + MENU_REPEAT_RATE_MS;
        }
        if (pressed & 2) {
            move_down = true;
            next_down_repeat = now + MENU_REPEAT_DELAY_MS;
        } else if ((cur & 2) && now >= next_down_repeat) {
            move_down = true;
            next_down_repeat = now + MENU_REPEAT_RATE_MS;
        }

        if (move_up && sel > 0) {
            sel--;
            RENDER_ROWS();
        }
        if (move_down && sel < count - 1) {
            sel++;
            RENDER_ROWS();
        }
        if (pressed & (4 | 8)) {
            ESP_LOGI(TAG, "Selected: %s", names[sel]);
            break;
        }
    }

#undef RENDER_ROWS

    lv_obj_clean(scr);
    lv_deinit();
    return sel;
}

/* Slot picker — runs inside an already-initialized LVGL context.
   Returns 0-9, or -1 if B is pressed (cancel → resume).
   rom_path: full path to the ROM file (used to find save files). */
static int pick_slot(const char *action_label, const char *rom_path)
{
    /* Extract stem (filename without directory and extension) */
    const char *slash = strrchr(rom_path, '/');
    const char *base  = slash ? slash + 1 : rom_path;
    char stem[256];
    strncpy(stem, base, 255);
    stem[255] = '\0';
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    /* Build labels: "Slot N  [SAVED]" or "Slot N" */
    char slot_labels[10][32];
    for (int i = 0; i < 10; i++) {
        char save_path[PATH_MAX + 1];
        snprintf(save_path, sizeof(save_path), SD_SAVE_DIR "/%s.ss%d", stem, i);
        struct stat st;
        if (stat(save_path, &st) == 0)
            snprintf(slot_labels[i], sizeof(slot_labels[i]), "Slot %d  [SAVED]", i);
        else
            snprintf(slot_labels[i], sizeof(slot_labels[i]), "Slot %d", i);
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    /* Title bar */
    lv_obj_t *title_bar = lv_obj_create(scr);
    lv_obj_set_size(title_bar, DISP_W, 28);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_make(80, 10, 30), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, action_label);
    lv_obj_set_style_text_color(title, lv_color_make(255, 210, 0), 0);
    lv_obj_center(title);

    /* Slot list */
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

    lv_obj_t *sbtns[10];
    for (int i = 0; i < 10; i++) {
        sbtns[i] = lv_list_add_btn(list, NULL, slot_labels[i]);
        lv_obj_set_style_bg_color(sbtns[i], lv_color_make(15, 15, 40), 0);
        lv_obj_set_style_bg_opa(sbtns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(sbtns[i], lv_color_make(200, 200, 200), 0);
        lv_obj_set_style_border_width(sbtns[i], 0, 0);
        lv_obj_clear_flag(sbtns[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* Hint bar */
    lv_obj_t *hint_bar = lv_obj_create(scr);
    lv_obj_set_size(hint_bar, DISP_W, 24);
    lv_obj_set_pos(hint_bar, 0, DISP_H - 24);
    lv_obj_set_style_bg_color(hint_bar, lv_color_make(80, 10, 30), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_radius(hint_bar, 0, 0);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_clear_flag(hint_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(hint_bar);
    lv_label_set_text(hint, "UP/DOWN: move   A: confirm   B: cancel");
    lv_obj_set_style_text_color(hint, lv_color_make(160, 160, 160), 0);
    lv_obj_center(hint);

#define SHIGHLIGHT(i, on) do { \
    lv_obj_set_style_bg_color(sbtns[i], \
        (on) ? lv_color_make(140, 10, 40) : lv_color_make(15, 15, 40), 0); \
    lv_obj_set_style_text_color(sbtns[i], \
        (on) ? lv_color_make(255, 230, 0) : lv_color_make(200, 200, 200), 0); \
} while (0)

    int sel = 0;
    SHIGHLIGHT(sel, true);
    lv_obj_invalidate(scr);
    for (int i = 0; i < 5; i++) lvgl_tick();

    int prev = 0;
    if (gpio_get_level(BTN_UP)    == 0) prev |= 1;
    if (gpio_get_level(BTN_DOWN)  == 0) prev |= 2;
    if (gpio_get_level(BTN_A)     == 0) prev |= 4;
    if (gpio_get_level(BTN_START) == 0) prev |= 8;
    if (gpio_get_level(BTN_B)     == 0) prev |= 16;

    int result = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();

        int cur = 0;
        if (gpio_get_level(BTN_UP)    == 0) cur |= 1;
        if (gpio_get_level(BTN_DOWN)  == 0) cur |= 2;
        if (gpio_get_level(BTN_A)     == 0) cur |= 4;
        if (gpio_get_level(BTN_START) == 0) cur |= 8;
        if (gpio_get_level(BTN_B)     == 0) cur |= 16;

        int pressed = cur & ~prev;
        prev = cur;

        if ((pressed & 1) && sel > 0) {
            SHIGHLIGHT(sel, false); sel--; SHIGHLIGHT(sel, true);
            lv_obj_scroll_to_view(sbtns[sel], LV_ANIM_ON);
        }
        if ((pressed & 2) && sel < 9) {
            SHIGHLIGHT(sel, false); sel++; SHIGHLIGHT(sel, true);
            lv_obj_scroll_to_view(sbtns[sel], LV_ANIM_ON);
        }
        if (pressed & (4 | 8)) { result = sel; break; }
        if (pressed & 16)       { result = -1;  break; }
    }

#undef SHIGHLIGHT
    return result;
}

pause_result_t menu_pause(const char *rom_path)
{
    lvgl_init();

    /* ── Screen ───────────────────────────────────────────────────────── */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ── Title bar ────────────────────────────────────────────────────── */
    lv_obj_t *title_bar = lv_obj_create(scr);
    lv_obj_set_size(title_bar, DISP_W, 28);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_make(80, 10, 30), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_bar);
    lv_label_set_text(title, "PAUSED");
    lv_obj_set_style_text_color(title, lv_color_make(255, 210, 0), 0);
    lv_obj_center(title);

    /* ── Option list ──────────────────────────────────────────────────── */
    static const char *option_labels[] = {
        "Resume",
        "Save State",
        "Load State",
        "Back to ROM menu",
        "Sleep",
    };
    const int NUM_OPTIONS = 5;

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

    lv_obj_t *btns[5];
    for (int i = 0; i < NUM_OPTIONS; i++) {
        btns[i] = lv_list_add_btn(list, NULL, option_labels[i]);
        lv_obj_set_style_bg_color(btns[i], lv_color_make(15, 15, 40), 0);
        lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btns[i], lv_color_make(200, 200, 200), 0);
        lv_obj_set_style_border_width(btns[i], 0, 0);
        lv_obj_clear_flag(btns[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* ── Hint bar ─────────────────────────────────────────────────────── */
    lv_obj_t *hint_bar = lv_obj_create(scr);
    lv_obj_set_size(hint_bar, DISP_W, 24);
    lv_obj_set_pos(hint_bar, 0, DISP_H - 24);
    lv_obj_set_style_bg_color(hint_bar, lv_color_make(80, 10, 30), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_radius(hint_bar, 0, 0);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_clear_flag(hint_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(hint_bar);
    lv_label_set_text(hint, "UP/DOWN: move   A: select   B: resume");
    lv_obj_set_style_text_color(hint, lv_color_make(160, 160, 160), 0);
    lv_obj_center(hint);

    /* ── Highlight helper ─────────────────────────────────────────────── */
    int sel = 0;
#define PHIGHLIGHT(i, on) do { \
    lv_obj_set_style_bg_color(btns[i], \
        (on) ? lv_color_make(140, 10, 40)  : lv_color_make(15, 15, 40), 0); \
    lv_obj_set_style_text_color(btns[i], \
        (on) ? lv_color_make(255, 230, 0)  : lv_color_make(200, 200, 200), 0); \
} while (0)

    PHIGHLIGHT(sel, true);
    lv_obj_invalidate(scr);
    for (int i = 0; i < 5; i++) lvgl_tick();

    /* ── Button polling loop ──────────────────────────────────────────── */
    int prev = 0;
    if (gpio_get_level(BTN_UP)    == 0) prev |= 1;
    if (gpio_get_level(BTN_DOWN)  == 0) prev |= 2;
    if (gpio_get_level(BTN_A)     == 0) prev |= 4;
    if (gpio_get_level(BTN_START) == 0) prev |= 8;
    if (gpio_get_level(BTN_B)     == 0) prev |= 16;

    pause_result_t result = { PAUSE_RESUME, 0 };
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();

        int cur = 0;
        if (gpio_get_level(BTN_UP)    == 0) cur |= 1;
        if (gpio_get_level(BTN_DOWN)  == 0) cur |= 2;
        if (gpio_get_level(BTN_A)     == 0) cur |= 4;
        if (gpio_get_level(BTN_START) == 0) cur |= 8;
        if (gpio_get_level(BTN_B)     == 0) cur |= 16;

        int pressed = cur & ~prev;
        prev = cur;

        if ((pressed & 1) && sel > 0) {
            PHIGHLIGHT(sel, false); sel--; PHIGHLIGHT(sel, true);
        }
        if ((pressed & 2) && sel < NUM_OPTIONS - 1) {
            PHIGHLIGHT(sel, false); sel++; PHIGHLIGHT(sel, true);
        }
        if (pressed & (4 | 8)) {
            if (sel == PAUSE_SAVE || sel == PAUSE_LOAD) {
                /* Show slot picker within same LVGL context */
                const char *picker_title = (sel == PAUSE_SAVE)
                    ? "SAVE STATE — Select Slot"
                    : "LOAD STATE — Select Slot";
                int slot = pick_slot(picker_title, rom_path);
                if (slot >= 0) {
                    result.action = (pause_action_t)sel;
                    result.slot   = slot;
                    break;
                }
                /* B pressed in slot picker → resume */
                result.action = PAUSE_RESUME;
                break;
            }
            result.action = (pause_action_t)sel;
            break;
        }
        if (pressed & 16) {
            result.action = PAUSE_RESUME;
            break;
        }
    }

#undef PHIGHLIGHT

    lv_obj_clean(scr);
    lv_deinit();
    return result;
}
