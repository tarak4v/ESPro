/**
 * @file sd_log.c
 * @brief Persistent exception/event logging via SPIFFS on internal flash.
 *
 * The board has no SD card slot, so we use a SPIFFS partition
 * named "storage" on the internal flash.  Logs are written to
 * /log/exceptions.txt with timestamps.
 */

#include "sd_log.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "sd_log";
static bool s_mounted = false;

void sd_log_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/log",
        .partition_label = "storage",
        .max_files       = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return;
    }

    s_mounted = true;
    sd_log_write("System boot");

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "Log storage mounted — %d/%d bytes used", (int)used, (int)total);
}

void sd_log_write(const char *message)
{
    if (!s_mounted || !message) return;

    FILE *f = fopen("/log/exceptions.txt", "a");
    if (!f) return;

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
            ti.tm_hour, ti.tm_min, ti.tm_sec,
            message);
    fclose(f);
}
