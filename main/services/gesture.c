/**
 * @file gesture.c
 * @brief IMU gesture state machine — double-tap and forward tilt.
 *
 * Double-tap: two accel spikes >2g within 400 ms.
 * Forward-tilt: pitch >30° sustained for 500 ms (~25 samples @ 50 Hz).
 */

#include "gesture.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "gesture";

/* ── Double-tap detection ─────────────────────────────────── */
#define TAP_THRESHOLD_G   2.0f
#define TAP_WINDOW_US     400000   /* 400 ms window for second tap */
#define TAP_COOLDOWN_US   600000   /* 600 ms cooldown after detection */

static int64_t s_first_tap_time  = 0;
static bool    s_awaiting_second = false;
static int64_t s_tap_cooldown_until = 0;

/* ── Forward-tilt detection ───────────────────────────────── */
#define TILT_ANGLE_DEG    30.0f
#define TILT_SUSTAIN_CNT  25       /* 25 samples @ 50 Hz = 500 ms */
#define TILT_COOLDOWN_US  2000000  /* 2 s cooldown */

static int     s_tilt_count = 0;
static int64_t s_tilt_cooldown_until = 0;

void gesture_init(void)
{
    s_first_tap_time     = 0;
    s_awaiting_second    = false;
    s_tap_cooldown_until = 0;
    s_tilt_count         = 0;
    s_tilt_cooldown_until = 0;
}

gesture_t gesture_feed(float ax, float ay, float az)
{
    int64_t now = esp_timer_get_time();

    float mag = sqrtf(ax * ax + ay * ay + az * az);

    /* ── Double-tap ─────────────────────────── */
    if (now > s_tap_cooldown_until) {
        bool spike = (mag > TAP_THRESHOLD_G);

        if (spike && !s_awaiting_second) {
            s_first_tap_time  = now;
            s_awaiting_second = true;
        } else if (spike && s_awaiting_second) {
            if (now - s_first_tap_time < TAP_WINDOW_US) {
                s_awaiting_second    = false;
                s_tap_cooldown_until = now + TAP_COOLDOWN_US;
                ESP_LOGI(TAG, "Double-tap detected");
                return GESTURE_DOUBLE_TAP;
            }
            /* Too late — treat as new first tap */
            s_first_tap_time = now;
        }

        /* Timeout first tap */
        if (s_awaiting_second && (now - s_first_tap_time > TAP_WINDOW_US))
            s_awaiting_second = false;
    }

    /* ── Forward tilt ───────────────────────── */
    if (now > s_tilt_cooldown_until) {
        /* Pitch angle: atan2(ax, sqrt(ay²+az²)) */
        float pitch_rad = atan2f(ax, sqrtf(ay * ay + az * az));
        float pitch_deg = pitch_rad * 57.2958f;  /* rad to deg */

        if (pitch_deg > TILT_ANGLE_DEG) {
            s_tilt_count++;
            if (s_tilt_count >= TILT_SUSTAIN_CNT) {
                s_tilt_count = 0;
                s_tilt_cooldown_until = now + TILT_COOLDOWN_US;
                ESP_LOGI(TAG, "Forward tilt detected");
                return GESTURE_FORWARD_TILT;
            }
        } else {
            s_tilt_count = 0;
        }
    }

    return GESTURE_NONE;
}
