/**
 * @file wellness.c
 * @brief Pedometer using peak-detection on accel magnitude + NVS persistence.
 */

#include "wellness.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <time.h>
#include <math.h>

static const char *TAG = "wellness";

#define NVS_NS          "wellness"
#define STEP_GOAL       6000
#define STEP_THRESHOLD  1.15f   /* g threshold for step peak */
#define STEP_COOLDOWN   8       /* samples between steps (~160 ms @ 50 Hz) */
#define ACTIVE_THRESH   1.05f   /* avg accel above this = active */

wellness_t g_wellness = {0};

/* Peak-detection state */
static float   s_prev_mag     = 1.0f;
static float   s_prev_prev    = 1.0f;
static int     s_cooldown     = 0;
static uint8_t s_active_acc   = 0;   /* accumulator for active-minute detection */
static uint32_t s_sample_cnt  = 0;

void wellness_init(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t day = 0;
        uint32_t steps = 0;
        uint16_t active = 0;
        nvs_get_u8(h, "day", &day);
        nvs_get_u32(h, "steps", &steps);
        nvs_get_u16(h, "active", &active);
        nvs_close(h);

        if (day == ti.tm_mday) {
            g_wellness.steps      = steps;
            g_wellness.active_min = active;
            ESP_LOGI(TAG, "Resumed: %lu steps, %d active min",
                     (unsigned long)steps, active);
        } else {
            ESP_LOGI(TAG, "New day — resetting wellness counters");
        }
    }
    g_wellness.last_reset_day = ti.tm_mday;
}

void wellness_feed_accel(float accel_mag)
{
    s_sample_cnt++;

    /* Step detection — look for peak in accel magnitude */
    if (s_cooldown > 0) {
        s_cooldown--;
    } else {
        if (s_prev_mag > STEP_THRESHOLD &&
            s_prev_mag > s_prev_prev &&
            s_prev_mag > accel_mag)
        {
            g_wellness.steps++;
            s_cooldown = STEP_COOLDOWN;
        }
    }
    s_prev_prev = s_prev_mag;
    s_prev_mag  = accel_mag;

    /* Active minute accumulation (~50 samples/sec × 60 = 3000/min) */
    if (accel_mag > ACTIVE_THRESH)
        s_active_acc++;

    if (s_sample_cnt % 3000 == 0) {
        if (s_active_acc > 1500)  /* >50% of samples above threshold */
            g_wellness.active_min++;
        s_active_acc = 0;
    }
}

void wellness_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "day",    g_wellness.last_reset_day);
        nvs_set_u32(h, "steps", g_wellness.steps);
        nvs_set_u16(h, "active", g_wellness.active_min);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint8_t wellness_score(void)
{
    uint32_t pct = (g_wellness.steps * 100) / STEP_GOAL;
    return (pct > 100) ? 100 : (uint8_t)pct;
}
