/**
 * @file app_manager.c
 * @brief Mode / screen manager with touch-swipe + BOOT button navigation.
 *
 * Swipe detection is done directly in the touch read callback (main.c)
 * which fires every LVGL tick (~5 ms).  BOOT button (GPIO 0) also cycles modes.
 */

#include "app_manager.h"
#include "hw_config.h"
#include "screen_clock.h"
#include "screen_menu.h"
#include "screen_settings.h"
#include "screen_tamafi.h"
#include "screen_wifi_cfg.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_mgr";

static app_mode_t current_mode = MODE_CLOCK;
lv_obj_t *g_pending_scr = NULL;

/* ── Transition animation state ───────────────────────────── */
#define ANIM_DURATION_MS  300
static bool        s_animating     = false;
static app_mode_t  s_destroy_mode  = MODE_COUNT;

/* BOOT button state for edge detection */
#define BOOT_BUTTON_GPIO  GPIO_NUM_0
#define BTN_DEBOUNCE_US   250000   /* 250 ms debounce */
static bool    boot_btn_prev      = true;       /* pull-up → idle HIGH */
static int64_t boot_btn_last_time = 0;

/* ── Screen create/destroy function table ─────────────────── */
typedef struct {
    void (*create)(void);
    void (*destroy)(void);
} screen_ops_t;

static const screen_ops_t screen_table[MODE_COUNT] = {
    [MODE_CLOCK]    = { screen_clock_create,    screen_clock_destroy    },
    [MODE_MENU]     = { screen_menu_create,     screen_menu_destroy     },
    [MODE_SETTINGS] = { screen_settings_create, screen_settings_destroy },
    [MODE_TAMAFI]   = { screen_tamafi_create,   screen_tamafi_destroy   },
    [MODE_WIFI_CFG] = { screen_wifi_cfg_create, screen_wifi_cfg_destroy },
};

/* ── Deferred old-screen destroy after animation ─────────── */
static void anim_done_timer_cb(lv_timer_t *timer)
{
    if (s_destroy_mode < MODE_COUNT) {
        screen_table[s_destroy_mode].destroy();
        s_destroy_mode = MODE_COUNT;
    }
    s_animating = false;
    lv_timer_del(timer);
}

/* ── Transition ───────────────────────────────────────────── */
static void load_screen(app_mode_t new_mode, lv_scr_load_anim_t anim)
{
    if (s_animating) return;

    app_mode_t old_mode = current_mode;
    current_mode = new_mode;

    /* Create new screen (sets g_pending_scr instead of loading). */
    screen_table[current_mode].create();

    if (g_pending_scr) {
        s_animating    = true;
        s_destroy_mode = old_mode;
        lv_scr_load_anim(g_pending_scr, anim, ANIM_DURATION_MS, 0, false);
        lv_timer_create(anim_done_timer_cb, ANIM_DURATION_MS + 50, NULL);
        g_pending_scr = NULL;
    } else {
        /* Fallback: no pending screen — destroy old immediately */
        screen_table[old_mode].destroy();
    }
    ESP_LOGI(TAG, "Switched to mode %d", current_mode);
}

static void navigate_next(void)
{
    if (s_animating) return;
    if (current_mode >= MODE_SWIPE_COUNT) {
        load_screen(MODE_MENU, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        return;
    }
    load_screen((current_mode + 1) % MODE_SWIPE_COUNT, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void navigate_prev(void)
{
    if (s_animating) return;
    if (current_mode >= MODE_SWIPE_COUNT) {
        load_screen(MODE_MENU, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        return;
    }
    load_screen(current_mode == 0 ? MODE_SWIPE_COUNT - 1 : current_mode - 1,
                LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

/* ── Public API ───────────────────────────────────────────── */
void app_manager_init(lv_indev_t *touch_indev)
{
    (void)touch_indev;
    current_mode = MODE_CLOCK;

    /* Configure BOOT button (GPIO 0) as input */
    gpio_config_t btn_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);
    boot_btn_prev      = gpio_get_level(BOOT_BUTTON_GPIO);
    boot_btn_last_time = esp_timer_get_time();

    /* Create the initial clock screen (no animation on boot) */
    screen_table[MODE_CLOCK].create();
    if (g_pending_scr) {
        lv_disp_load_scr(g_pending_scr);
        g_pending_scr = NULL;
    }

    ESP_LOGI(TAG, "App manager initialised — starting with CLOCK");
}

void app_manager_update(void)
{
    /* ── Process swipe from touch callback (main.c) ── */
    int8_t swipe = get_pending_swipe();
    if (swipe < 0) {
        navigate_next();       /* swipe left  → next mode */
    } else if (swipe > 0) {
        navigate_prev();       /* swipe right → prev mode */
    }

    /* ── BOOT button: falling edge → next mode (debounced) ── */
    bool btn_now = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now  = esp_timer_get_time();
    if (!btn_now && boot_btn_prev && (now - boot_btn_last_time > BTN_DEBOUNCE_US)) {
        boot_btn_last_time = now;
        navigate_next();
    }
    boot_btn_prev = btn_now;

    /* ── Let current screen update (clock ticks, IMU, etc.) ── */
    switch (current_mode) {
        case MODE_CLOCK:    screen_clock_update();    break;
        case MODE_MENU:     screen_menu_update();     break;
        case MODE_TAMAFI:   screen_tamafi_update();   break;
        case MODE_WIFI_CFG: screen_wifi_cfg_update(); break;
        default: break;
    }
}

void app_manager_set_mode(app_mode_t mode)
{
    if (mode >= MODE_COUNT || mode == current_mode || s_animating) return;
    lv_scr_load_anim_t anim = (mode < current_mode)
        ? LV_SCR_LOAD_ANIM_MOVE_RIGHT : LV_SCR_LOAD_ANIM_MOVE_LEFT;
    load_screen(mode, anim);
}

app_mode_t app_manager_get_mode(void)
{
    return current_mode;
}
