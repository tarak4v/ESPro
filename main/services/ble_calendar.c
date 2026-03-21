/**
 * @file ble_calendar.c
 * @brief Calendar event storage — BLE GATT write from phone + LittleFS cache.
 *
 * The phone companion app writes JSON events to a BLE GATT characteristic.
 * Format: {"t":"Meeting","h":14,"m":30,"d":21,"mo":3}
 *
 * For now, BLE GATT registration is deferred (requires integration with
 * the existing macropad BLE HID stack). Events can be added via
 * ble_calendar_add() from DeepSeek responses or loaded from LittleFS.
 */

#include "ble_calendar.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ble_cal";
#define EVENTS_PATH "/log/events.json"

cal_event_t g_cal_events[CAL_EVENT_MAX] = {0};
uint8_t     g_cal_count = 0;

/* ── LittleFS persistence ─────────────────────────────────── */
void ble_calendar_load(void)
{
    FILE *f = fopen(EVENTS_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No cached events");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }

    g_cal_count = 0;
    int n = cJSON_GetArraySize(arr);
    if (n > CAL_EVENT_MAX) n = CAL_EVENT_MAX;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *jt   = cJSON_GetObjectItem(item, "t");
        cJSON *jh   = cJSON_GetObjectItem(item, "h");
        cJSON *jm   = cJSON_GetObjectItem(item, "m");
        cJSON *jd   = cJSON_GetObjectItem(item, "d");
        cJSON *jmo  = cJSON_GetObjectItem(item, "mo");
        if (!jt || !cJSON_IsString(jt)) continue;

        cal_event_t *e = &g_cal_events[g_cal_count];
        snprintf(e->title, CAL_TITLE_LEN, "%s", jt->valuestring);
        e->hour   = jh  ? (uint8_t)jh->valueint  : 0;
        e->minute = jm  ? (uint8_t)jm->valueint  : 0;
        e->day    = jd  ? (uint8_t)jd->valueint  : 0;
        e->month  = jmo ? (uint8_t)jmo->valueint : 0;
        e->valid  = true;
        g_cal_count++;
    }

    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Loaded %d calendar events", g_cal_count);
}

void ble_calendar_save(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_cal_count; i++) {
        cal_event_t *e = &g_cal_events[i];
        if (!e->valid) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "t",  e->title);
        cJSON_AddNumberToObject(item, "h",  e->hour);
        cJSON_AddNumberToObject(item, "m",  e->minute);
        cJSON_AddNumberToObject(item, "d",  e->day);
        cJSON_AddNumberToObject(item, "mo", e->month);
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return;

    FILE *f = fopen(EVENTS_PATH, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
}

void ble_calendar_init(void)
{
    ble_calendar_load();

    /* Seed sample events if empty */
    if (g_cal_count == 0) {
        ble_calendar_add("Stand-up", 10, 0, 0, 0);
        ble_calendar_add("Lunch", 13, 0, 0, 0);
        ble_calendar_save();
    }
}

void ble_calendar_add(const char *title, uint8_t hour, uint8_t min,
                      uint8_t day, uint8_t month)
{
    if (g_cal_count >= CAL_EVENT_MAX) return;
    cal_event_t *e = &g_cal_events[g_cal_count];
    snprintf(e->title, CAL_TITLE_LEN, "%s", title);
    e->hour   = hour;
    e->minute = min;
    e->day    = day;
    e->month  = month;
    e->valid  = true;
    g_cal_count++;
}

const cal_event_t *ble_calendar_next_event(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    int now_min = ti.tm_hour * 60 + ti.tm_min;
    const cal_event_t *best = NULL;
    int best_diff = 9999;

    for (int i = 0; i < g_cal_count; i++) {
        cal_event_t *e = &g_cal_events[i];
        if (!e->valid) continue;

        /* If day/month specified, skip if not today */
        if (e->day > 0 && e->day != ti.tm_mday) continue;
        if (e->month > 0 && e->month != (ti.tm_mon + 1)) continue;

        int evt_min = e->hour * 60 + e->minute;
        int diff = evt_min - now_min;
        if (diff < -5) continue;  /* Already past by >5 min */
        if (diff < 0) diff = 0;   /* Currently happening */

        if (diff < best_diff) {
            best_diff = diff;
            best = e;
        }
    }
    return best;
}
