/**
 * @file wellness.h
 * @brief Step counter + activity tracking from IMU accelerometer.
 */
#ifndef WELLNESS_H
#define WELLNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t steps;              /* daily step count */
    uint16_t active_min;         /* active minutes today */
    uint8_t  last_reset_day;     /* day-of-month of last reset */
} wellness_t;

extern wellness_t g_wellness;

/** Initialise wellness — load today's count from NVS. */
void wellness_init(void);

/** Feed a new accel sample (magnitude in g). Call at ~50 Hz. */
void wellness_feed_accel(float accel_mag);

/** Save current state to NVS. Call periodically (e.g. every 60 s). */
void wellness_save(void);

/** Get a wellness score 0–100 (based on step goal). */
uint8_t wellness_score(void);

#ifdef __cplusplus
}
#endif

#endif /* WELLNESS_H */
