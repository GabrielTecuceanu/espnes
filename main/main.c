/* espNES — main entry point */
#include "nofrendo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espnes";

// ROM path passed to osd_main() inside NOFRENDO
const char *nes_rom_path = "/sdcard/rom.nes";

void app_main(void) {
    ESP_LOGI(TAG, "espNES starting — ROM: %s", nes_rom_path);
    nofrendo_main(0, NULL);
    ESP_LOGE(TAG, "nofrendo_main returned unexpectedly");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
