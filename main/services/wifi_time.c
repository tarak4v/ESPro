/**
 * @file wifi_time.c
 * @brief WiFi STA connection + SNTP time sync → PCF85063 RTC write.
 */

#include "wifi_time.h"
#include "wifi_prov.h"
#include "hw_config.h"
#include "i2c_bsp.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_time";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define WIFI_MAX_RETRY  10

static volatile bool s_wifi_connected = false;
static char s_current_ssid[33] = WIFI_SSID;

/* ── BCD helpers (same as screen_clock, kept local) ───────── */
static inline uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/* ── WiFi event handler ──────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi connect failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Write system time to PCF85063 RTC ───────────────────── */
static void write_time_to_rtc(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year < (2024 - 1900)) {
        ESP_LOGW(TAG, "System time still not set, skipping RTC write");
        return;
    }

    uint8_t regs[7];
    regs[0] = dec2bcd(ti.tm_sec);
    regs[1] = dec2bcd(ti.tm_min);
    regs[2] = dec2bcd(ti.tm_hour);
    regs[3] = dec2bcd(ti.tm_mday);
    regs[4] = ti.tm_wday;                    /* 0=SUN */
    regs[5] = dec2bcd(ti.tm_mon + 1);        /* tm_mon is 0-based */
    regs[6] = dec2bcd(ti.tm_year - 100);     /* years since 2000 */

    if (i2c_writr_buff(rtc_dev_handle, PCF85063_REG_SECONDS, regs, 7) == 0) {
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d (wday=%d)",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_wday);
    } else {
        ESP_LOGE(TAG, "Failed to write time to RTC");
    }
}

/* ── SNTP time-sync callback ─────────────────────────────── */
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronised");
    write_time_to_rtc();
}

/* ── Public API ───────────────────────────────────────────── */
void wifi_time_init(void)
{
    /* NVS (required by WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Network interface + event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* WiFi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t inst_any_id;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    /* Configure STA — prefer NVS-stored credentials, fall back to defaults */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    char nvs_ssid[33] = "", nvs_pass[65] = "";
    if (wifi_prov_load_creds(nvs_ssid, sizeof(nvs_ssid),
                             nvs_pass, sizeof(nvs_pass))) {
        strncpy((char *)wifi_cfg.sta.ssid, nvs_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, nvs_pass, sizeof(wifi_cfg.sta.password) - 1);
        strncpy(s_current_ssid, nvs_ssid, sizeof(s_current_ssid) - 1);
    } else {
        strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);
        strncpy(s_current_ssid, WIFI_SSID, sizeof(s_current_ssid) - 1);
    }
    if (!wifi_cfg.sta.password[0])
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, connecting to \"%s\"...", s_current_ssid);

    /* Wait for connection (max ~20 s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected — starting SNTP");

        /* Configure SNTP */
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        esp_sntp_init();

        /* Wait for NTP sync (max 15 s) */
        int retry = 0;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retry < 30) {
            vTaskDelay(pdMS_TO_TICKS(500));
            retry++;
        }
        if (sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
            ESP_LOGI(TAG, "NTP sync complete");
        } else {
            ESP_LOGW(TAG, "NTP sync timed out — RTC keeps its current time");
        }
    } else {
        ESP_LOGW(TAG, "WiFi connection failed — RTC keeps its current time");
    }
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

const char *wifi_get_current_ssid(void)
{
    return s_current_ssid;
}

void wifi_reset_retry(void)
{
    s_retry_num = 0;
}
