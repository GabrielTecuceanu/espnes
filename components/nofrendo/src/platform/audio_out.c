/* audio_out.c — MAX98357A via I2S, new esp_driver_i2s API */
#include "audio_out.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define PIN_BCLK  5
#define PIN_WS    6
#define PIN_DOUT  4

#define SILENCE_SAMPLES 64

static i2s_chan_handle_t tx_chan = NULL;

void audio_init(int sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
}

void audio_write(const int16_t *buf, int n_samples)
{
    if (!tx_chan) return;
    size_t written;
    i2s_channel_write(tx_chan, buf, n_samples * sizeof(int16_t), &written, portMAX_DELAY);
}

void audio_silence(void)
{
    if (!tx_chan) return;
    static const int16_t zeros[SILENCE_SAMPLES] = {0};
    size_t written;
    i2s_channel_write(tx_chan, zeros, sizeof(zeros), &written, 0);
}
