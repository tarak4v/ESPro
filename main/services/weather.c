/**
 * @file weather.c
 * @brief Periodic weather fetch from OpenWeatherMap (HTTP).
 *
 * Runs a FreeRTOS task that queries the API every 10 minutes
 * and stores the latest temperature / description for the UI.
 */

#include "weather.h"
#include "hw_config.h"
#include "wifi_time.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "weather";

static weather_data_t   s_weather = {0};
static SemaphoreHandle_t s_mutex  = NULL;

#define HTTP_BUF_SIZE       2048
#define WEATHER_UPDATE_MS   (30 * 60 * 1000)  /* 30 minutes */

/* ── Fetch + parse ────────────────────────────────────────── */

static void fetch_weather(void)
{
    if (!wifi_is_connected()) return;

    char url[256];
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/weather"
             "?q=%s&appid=%s&units=metric",
             WEATHER_CITY, WEATHER_API_KEY);

    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) return;

    esp_http_client_config_t cfg = {
        .url    = url,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);

    int total = 0, n;
    while ((n = esp_http_client_read(client, buf + total,
                                     HTTP_BUF_SIZE - 1 - total)) > 0) {
        total += n;
        if (total >= HTTP_BUF_SIZE - 1) break;
    }
    buf[total] = '\0';

    if (total == 0) goto cleanup;

    cJSON *root = cJSON_Parse(buf);
    if (!root) goto cleanup;

    weather_data_t w = {0};

    cJSON *main_obj = cJSON_GetObjectItem(root, "main");
    if (main_obj) {
        cJSON *t = cJSON_GetObjectItem(main_obj, "temp");
        if (t && cJSON_IsNumber(t)) w.temp = (float)t->valuedouble;
        cJSON *h = cJSON_GetObjectItem(main_obj, "humidity");
        if (h && cJSON_IsNumber(h)) w.humidity = (int)h->valuedouble;
    }

    cJSON *wa = cJSON_GetObjectItem(root, "weather");
    if (wa && cJSON_IsArray(wa)) {
        cJSON *first = cJSON_GetArrayItem(wa, 0);
        if (first) {
            cJSON *m = cJSON_GetObjectItem(first, "main");
            if (m && cJSON_IsString(m))
                strncpy(w.description, m->valuestring,
                        sizeof(w.description) - 1);
        }
    }

    w.valid = true;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200))) {
        s_weather = w;
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "Weather: %.1f C, %s, %d%% humidity",
             w.temp, w.description, w.humidity);
    cJSON_Delete(root);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
}

/* ── Background task ──────────────────────────────────────── */

static void weather_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));   /* let WiFi settle */
    for (;;) {
        fetch_weather();
        vTaskDelay(pdMS_TO_TICKS(WEATHER_UPDATE_MS));
    }
}

/* ── Public API ───────────────────────────────────────────── */

void weather_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    xTaskCreate(weather_task, "weather", 8192, NULL, 2, NULL);
    ESP_LOGI(TAG, "Weather task started (city: %s)", WEATHER_CITY);
}

weather_data_t weather_get(void)
{
    weather_data_t w = {0};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100))) {
        w = s_weather;
        xSemaphoreGive(s_mutex);
    }
    return w;
}
