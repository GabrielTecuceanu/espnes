/* espNES - main entry point */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nofrendo.h"

static const char* TAG = "espnes";

void app_main(void) {
    ESP_LOGI(TAG, "espNES starting");
    nofrendo_main(0, NULL);
    ESP_LOGE(TAG, "nofrendo_main returned unexpectedly");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
