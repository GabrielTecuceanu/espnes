/* display.c */
#include "display.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES LEDC_TIMER_8_BIT

#define PIN_SCK 11
#define PIN_MOSI 10
#define PIN_MISO 13
#define PIN_CS 18
#define PIN_DC 9
#define PIN_RST 8
#define PIN_BL 12

#define LCD_W 320
#define LCD_H 240
#define SPI_CLK_HZ (80 * 1000 * 1000)

static esp_lcd_panel_handle_t panel = NULL;

void display_set_backlight(int level) {
    int duty = 5 + level * 25;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

void display_sleep(void) {
    ledc_stop(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, 0);
    gpio_hold_en(PIN_BL);
}

void display_wake(int level) {
    gpio_hold_dis(PIN_BL);
    display_set_backlight(level);
}

void display_init(void) {
    gpio_hold_dis(PIN_BL);

    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ledc_timer_config(&lt);

    ledc_channel_config_t lc = {
        .gpio_num = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ledc_channel_config(&lc);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 16 * sizeof(uint16_t),
    };

    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    
    esp_lcd_new_panel_st7789(io, &dev_cfg, &panel);

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, 0);

    display_clear_black();
    esp_lcd_panel_disp_on_off(panel, true);
    display_set_backlight(10);
}

void display_clear_black(void) {
    static uint16_t black[LCD_W * 16];
    for (int y = 0; y < LCD_H; y += 16)
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 16, black);
}

void display_push_frame(int x, int y, int w, int h, const uint16_t* rgb565) {
    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, rgb565);
}
