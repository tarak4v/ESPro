/**
 * @file gesture.h
 * @brief IMU gesture detection — double-tap, forward-tilt.
 */
#ifndef GESTURE_H
#define GESTURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_DOUBLE_TAP,      /* snooze timer */
    GESTURE_FORWARD_TILT,    /* acknowledge alert */
} gesture_t;

/** Initialise gesture detection state. */
void gesture_init(void);

/**
 * Feed raw accel data (in g). Call at ~50 Hz.
 * Returns detected gesture or GESTURE_NONE.
 */
gesture_t gesture_feed(float ax, float ay, float az);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_H */
