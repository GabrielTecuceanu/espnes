/* sd.c — FAT mount on shared SPI2 + iNES ROM loader */
#include "sd.h"
#include <stdbool.h>   // must come before sdmmc headers; sd_protocol_types.h uses bool without including stdbool.h
#include <dirent.h>
#include <string.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <sys/stat.h>

#define PIN_CS_SD   17
#define MOUNT_POINT "/sdcard"
#define TAG         "sd"

static sdmmc_card_t *card = NULL;

// SPI2 bus is already initialized by display_init(); returning ESP_OK
// prevents esp_vfs_fat_sdspi_mount from double-initializing the bus.
static esp_err_t spi_bus_already_init(void) { return ESP_OK; }

int sd_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.init = spi_bus_already_init;   // bus already up from display_init()

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = PIN_CS_SD;
    dev_cfg.host_id = SPI2_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_cfg,
                                             &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        return -1;
    }
    ESP_LOGI(TAG, "SD mounted at %s", MOUNT_POINT);

    /* Create required directories if they don't exist yet */
    mkdir(SD_ROM_DIR,  0775);
    mkdir(SD_SAVE_DIR, 0775);

    return 0;
}

uint8_t *sd_load_rom(const char *path, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat failed: %s", path);
        return NULL;
    }
    size_t size = (size_t)st.st_size;

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for ROM (%u bytes)", (unsigned)size);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return NULL; }

    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size) { free(buf); return NULL; }

    if (size < 16 || buf[0] != 0x4E || buf[1] != 0x45 ||
        buf[2] != 0x53 || buf[3] != 0x1A) {
        ESP_LOGE(TAG, "not a valid iNES file");
        free(buf);
        return NULL;
    }

    ESP_LOGI(TAG, "ROM loaded: %s (%u bytes)", path, (unsigned)size);
    *out_size = size;
    return buf;
}

int sd_list_roms(char names[][SD_NAME_LEN], int max_count)
{
    DIR *dir = opendir(SD_ROM_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while (count < max_count && (ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;  // skip hidden files and macOS ._* metadata
        size_t len = strlen(name);
        if (len > 4 && strcasecmp(name + len - 4, ".nes") == 0) {
            strncpy(names[count], name, SD_NAME_LEN - 1);
            names[count][SD_NAME_LEN - 1] = '\0';
            count++;
        }
    }
    closedir(dir);

    // Alphabetical sort
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcasecmp(names[i], names[j]) > 0) {
                char tmp[SD_NAME_LEN];
                memcpy(tmp, names[i], SD_NAME_LEN);
                memcpy(names[i], names[j], SD_NAME_LEN);
                memcpy(names[j], tmp, SD_NAME_LEN);
            }

    ESP_LOGI(TAG, "Found %d ROM(s) on SD", count);
    return count;
}
