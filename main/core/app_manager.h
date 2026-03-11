#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Screen mode IDs — order matches swipe sequence.
 * Swipe left  → next mode
 * Swipe right → previous mode
 */
typedef enum {
    MODE_CLOCK = 0,
    MODE_MENU,
    MODE_SWIPE_COUNT,          /* number of modes in swipe cycle */
    MODE_SETTINGS = MODE_SWIPE_COUNT,
    MODE_TAMAFI,
    MODE_WIFI_CFG,
    MODE_COUNT
} app_mode_t;

/** Initialise the app manager; creates the first screen (clock). */
void app_manager_init(lv_indev_t *touch_indev);

/** Called periodically from the app update task. */
void app_manager_update(void);

/** Switch to a given mode. */
void app_manager_set_mode(app_mode_t mode);

/** Get current mode. */
app_mode_t app_manager_get_mode(void);

/* LVGL lock/unlock (defined in main.c) */
extern bool lvgl_lock(int timeout_ms);
extern void lvgl_unlock(void);

/* Touch swipe result (defined in main.c) */
extern int8_t get_pending_swipe(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MANAGER_H */
