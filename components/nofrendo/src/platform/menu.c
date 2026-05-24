/* menu.c */
#include "menu.h"

#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static void cpu_freq_mhz(int mhz) {
    esp_pm_config_t cfg = {.max_freq_mhz = mhz, .min_freq_mhz = mhz, .light_sleep_enable = false};
    esp_pm_configure(&cfg);
}

#define TAG "menu"

#define BTN_UP 47
#define BTN_DOWN 39
#define BTN_A 42
#define BTN_B 41
#define BTN_START 2

#define DISP_W 320
#define DISP_H 240
#define BUF_ROWS 10
#define BUF_SIZE (DISP_W * BUF_ROWS)
#define MENU_ROW_H 22
#define MENU_VISIBLE_ROWS ((DISP_H - 28 - 24 - 4) / MENU_ROW_H)
#define MENU_REPEAT_DELAY_MS 300
#define MENU_REPEAT_RATE_MS 70

static lv_color_t lvgl_buf[BUF_SIZE];
static uint16_t spi_buf[BUF_SIZE];

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_disp_t* disp;
static uint32_t lvgl_last_ms = 0;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    int n = w * h;
    uint16_t* src = (uint16_t*)pixels;
    for (int i = 0; i < n; i++) spi_buf[i] = (src[i] >> 8) | (src[i] << 8);

    display_push_frame(area->x1, area->y1, w, h, spi_buf);
    lv_disp_flush_ready(drv);
}

static void lvgl_init(void) {
    lvgl_last_ms = (uint32_t)(esp_timer_get_time() / 1000);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, BUF_SIZE);
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = DISP_W;
    disp_drv.ver_res = DISP_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp = lv_disp_drv_register(&disp_drv);
}

static void lvgl_tick(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    lv_tick_inc(now - lvgl_last_ms);
    lvgl_last_ms = now;
    lv_timer_handler();
}

static void make_bar(lv_obj_t* scr, int y, int h, lv_color_t bg, lv_color_t text_color,
                     const char* text) {
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_size(bar, DISP_W, h);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, bg, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_obj_center(lbl);
}

static lv_obj_t* make_list(lv_obj_t* scr) {
    lv_obj_t* list = lv_list_create(scr);
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
    return list;
}

static int read_menu_buttons(bool with_b) {
    int mask = 0;
    if (gpio_get_level(BTN_UP) == 0)
        mask |= 1;
    if (gpio_get_level(BTN_DOWN) == 0)
        mask |= 2;
    if (gpio_get_level(BTN_A) == 0)
        mask |= 4;
    if (gpio_get_level(BTN_START) == 0)
        mask |= 8;
    if (with_b && gpio_get_level(BTN_B) == 0)
        mask |= 16;
    return mask;
}

static void set_highlight(lv_obj_t* btn, bool on) {
    lv_obj_set_style_bg_color(btn, on ? lv_color_make(140, 10, 40) : lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_text_color(btn, on ? lv_color_make(255, 230, 0) : lv_color_make(200, 200, 200),
                                0);
}

int menu_select(const char names[][SD_NAME_LEN], int count) {
    if (count == 0)
        return -1;
    if (count == 1) {
        ESP_LOGI(TAG, "Auto-selecting: %s", names[0]);
        return 0;
    }

    cpu_freq_mhz(80);
    lvgl_init();

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    make_bar(scr, 0, 28, lv_color_make(30, 10, 80), lv_color_make(255, 210, 0),
             "espNES  -  SELECT ROM");

    lv_obj_t* rows[MENU_VISIBLE_ROWS];
    lv_obj_t* labels[MENU_VISIBLE_ROWS];
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

    make_bar(scr, DISP_H - 24, 24, lv_color_make(30, 10, 80), lv_color_make(160, 160, 160),
             "UP/DOWN: move   A/START: select");

    int sel = 0;
#define RENDER_ROWS()                                                                              \
    do {                                                                                           \
        int top = sel - (MENU_VISIBLE_ROWS / 2);                                                   \
        if (top < 0)                                                                               \
            top = 0;                                                                               \
        if (top > count - MENU_VISIBLE_ROWS)                                                       \
            top = count - MENU_VISIBLE_ROWS;                                                       \
        if (top < 0)                                                                               \
            top = 0;                                                                               \
        for (int i = 0; i < MENU_VISIBLE_ROWS; i++) {                                              \
            int idx = top + i;                                                                     \
            if (idx >= count) {                                                                    \
                lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);                                      \
                continue;                                                                          \
            }                                                                                      \
            lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);                                        \
            lv_label_set_text(labels[i], names[idx]);                                              \
            lv_label_set_long_mode(                                                                \
                labels[i], (idx == sel) ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);      \
            lv_obj_set_style_bg_color(                                                             \
                rows[i], (idx == sel) ? lv_color_make(0, 90, 210) : lv_color_make(15, 15, 40), 0); \
            lv_obj_set_style_text_color(                                                           \
                labels[i],                                                                         \
                (idx == sel) ? lv_color_make(255, 230, 0) : lv_color_make(200, 200, 200), 0);      \
        }                                                                                          \
    } while (0)

    RENDER_ROWS();

    lv_obj_invalidate(scr);
    for (int i = 0; i < 5; i++) lvgl_tick();

    int prev = read_menu_buttons(false);

    int64_t next_up_repeat = 0;
    int64_t next_down_repeat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();
        int64_t now = esp_timer_get_time() / 1000;

        int cur = read_menu_buttons(false);
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
    cpu_freq_mhz(240);
    return sel;
}

static int pick_slot(const char* action_label, const char* rom_path) {
    const char* slash = strrchr(rom_path, '/');
    const char* base = slash ? slash + 1 : rom_path;
    char stem[256];
    strncpy(stem, base, 255);
    stem[255] = '\0';
    char* dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';

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

    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    make_bar(scr, 0, 28, lv_color_make(80, 10, 30), lv_color_make(255, 210, 0), action_label);
    lv_obj_t* list = make_list(scr);

    lv_obj_t* sbtns[10];
    for (int i = 0; i < 10; i++) {
        sbtns[i] = lv_list_add_btn(list, NULL, slot_labels[i]);
        lv_obj_set_style_bg_color(sbtns[i], lv_color_make(15, 15, 40), 0);
        lv_obj_set_style_bg_opa(sbtns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(sbtns[i], lv_color_make(200, 200, 200), 0);
        lv_obj_set_style_border_width(sbtns[i], 0, 0);
        lv_obj_clear_flag(sbtns[i], LV_OBJ_FLAG_CLICKABLE);
    }

    make_bar(scr, DISP_H - 24, 24, lv_color_make(80, 10, 30), lv_color_make(160, 160, 160),
             "UP/DOWN: move   A: confirm   B: cancel");

    int sel = 0;
    set_highlight(sbtns[sel], true);
    lv_obj_invalidate(scr);
    for (int i = 0; i < 5; i++) lvgl_tick();

    int prev = read_menu_buttons(true);

    int result = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        lvgl_tick();

        int cur = read_menu_buttons(true);
        int pressed = cur & ~prev;
        prev = cur;

        if ((pressed & 1) && sel > 0) {
            set_highlight(sbtns[sel], false);
            sel--;
            set_highlight(sbtns[sel], true);
            lv_obj_scroll_to_view(sbtns[sel], LV_ANIM_ON);
        }
        if ((pressed & 2) && sel < 9) {
            set_highlight(sbtns[sel], false);
            sel++;
            set_highlight(sbtns[sel], true);
            lv_obj_scroll_to_view(sbtns[sel], LV_ANIM_ON);
        }
        if (pressed & (4 | 8)) {
            result = sel;
            break;
        }
        if (pressed & 16) {
            result = -1;
            break;
        }
    }

    return result;
}

pause_result_t menu_pause(const char* rom_path) {
    cpu_freq_mhz(80);
    lvgl_init();

    static const char* option_labels[] = {
        "Resume", "Save State", "Load State", "Back to ROM menu", "Sleep", "Memory Report",
    };

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(15, 15, 40), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    pause_result_t result = {PAUSE_RESUME, 0};
    int sel = 0;
    bool done = false;

    while (!done) {
        lv_obj_clean(scr);
        make_bar(scr, 0, 28, lv_color_make(80, 10, 30), lv_color_make(255, 210, 0), "PAUSED");

        lv_obj_t* list = make_list(scr);
        lv_obj_t* btns[6];
        for (int i = 0; i < 6; i++) {
            btns[i] = lv_list_add_btn(list, NULL, option_labels[i]);
            lv_obj_set_style_bg_color(btns[i], lv_color_make(15, 15, 40), 0);
            lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(btns[i], lv_color_make(200, 200, 200), 0);
            lv_obj_set_style_border_width(btns[i], 0, 0);
            lv_obj_clear_flag(btns[i], LV_OBJ_FLAG_CLICKABLE);
        }
        make_bar(scr, DISP_H - 24, 24, lv_color_make(80, 10, 30), lv_color_make(160, 160, 160),
                 "UP/DOWN: move   A: select   B: resume");

        set_highlight(btns[sel], true);
        lv_obj_invalidate(scr);
        for (int i = 0; i < 5; i++) lvgl_tick();

        int prev = read_menu_buttons(true);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(20));
            lvgl_tick();

            int cur = read_menu_buttons(true);
            int pressed = cur & ~prev;
            prev = cur;

            if ((pressed & 1) && sel > 0) {
                set_highlight(btns[sel], false);
                sel--;
                set_highlight(btns[sel], true);
                lv_obj_scroll_to_view(btns[sel], LV_ANIM_ON);
            }
            if ((pressed & 2) && sel < 5) {
                set_highlight(btns[sel], false);
                sel++;
                set_highlight(btns[sel], true);
                lv_obj_scroll_to_view(btns[sel], LV_ANIM_ON);
            }
            if (pressed & (4 | 8)) {
                if (sel == PAUSE_SAVE || sel == PAUSE_LOAD) {
                    const char* picker_title = (sel == PAUSE_SAVE) ? "SAVE STATE - Select Slot"
                                                                   : "LOAD STATE - Select Slot";
                    int slot = pick_slot(picker_title, rom_path);
                    if (slot >= 0) {
                        result.action = (pause_action_t)sel;
                        result.slot = slot;
                        done = true;
                    }

                    break;
                }
                result.action = (pause_action_t)sel;
                done = true;
                break;
            }
            if (pressed & 16) {
                result.action = PAUSE_RESUME;
                done = true;
                break;
            }
        }
    }

    lv_obj_clean(scr);
    lv_deinit();
    cpu_freq_mhz(240);
    return result;
}
