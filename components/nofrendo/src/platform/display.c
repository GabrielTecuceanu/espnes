/* display.c — ST7789V 320×240 via esp-lcd / SPI2 */
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SCK   11
#define PIN_MOSI  10
#define PIN_MISO  13
#define PIN_CS    18
#define PIN_DC     9
#define PIN_RST    8
#define PIN_BL    12

#define LCD_W     320
#define LCD_H     240
#define SPI_CLK_HZ (80 * 1000 * 1000)

static esp_lcd_panel_handle_t panel = NULL;

void display_init(void)
{
    // Backlight off during init to avoid white flash
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num   = PIN_SCK,
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 16 * sizeof(uint16_t),
    };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = SPI_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(io, &dev_cfg, &panel);

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, 0);

    // Fill black before turning on to avoid white flash
    static uint16_t black[LCD_W];
    // black[] is zero-initialized (static), so all pixels are 0x0000
    for (int y = 0; y < LCD_H; y++)
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 1, black);

    esp_lcd_panel_disp_on_off(panel, true);
    gpio_set_level(PIN_BL, 1);
}

void display_clear_black(void)
{
    static uint16_t black[LCD_W * 16]; /* zero-initialised = black */
    for (int y = 0; y < LCD_H; y += 16)
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 16, black);
}

void display_push_frame(int x, int y, int w, int h, const uint16_t *rgb565)
{
    esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, rgb565);
}
