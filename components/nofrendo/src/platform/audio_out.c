/* audio_out.c */
#include "audio_out.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

#define PIN_BCLK 5
#define PIN_WS 6
#define PIN_DOUT 4

#define WRITE_BUF_FRAMES 256

static i2s_chan_handle_t tx_chan = NULL;

void audio_init(int sample_rate) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),

        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = PIN_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {0},
            },
    };

    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
}

void audio_write(const int16_t* buf, int n_samples) {
    if (!tx_chan || n_samples <= 0)
        return;
    if (n_samples > WRITE_BUF_FRAMES)
        n_samples = WRITE_BUF_FRAMES;

    static int16_t stereo[WRITE_BUF_FRAMES * 2];
    for (int i = 0; i < n_samples; i++) {
        stereo[i * 2] = buf[i];
        stereo[i * 2 + 1] = buf[i];
    }

    size_t written;

    i2s_channel_write(tx_chan, stereo, n_samples * 4, &written, portMAX_DELAY);
}
