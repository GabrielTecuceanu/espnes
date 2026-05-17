/* espNES — NES emulator on ESP32-S3 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espnes";

void app_main(void) {
    ESP_LOGI(TAG, "espNES v0.1 — CPU 240MHz, flash 16MB");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
