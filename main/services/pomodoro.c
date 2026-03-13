/**
 * @file pomodoro.c
 * @brief Pomodoro timer — 25 min work / 5 min break with sound alert.
 *
 * Overlay on menu screen (640×172).  Shows large countdown timer,
 * session counter, and start/pause/reset controls.  Plays a short
 * melody when a period ends.  Persists session count to NVS.
 */

#include "pomodoro.h"
#include "hw_config.h"
#include "boot_sound.h"
#include "screen_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pomodoro";

/* ── Geometry ─────────────────────────────────────────────── */
#define GW  LCD_H_RES   /* 640 */
#define GH  LCD_V_RES   /* 172 */

/* ── Timer durations (seconds) ────────────────────────────── */
#define WORK_SECS    (25 * 60)
#define BREAK_SECS   ( 5 * 60)

/* ── NVS ──────────────────────────────────────────────────── */
#define NVS_NS       "settings"
#define NVS_KEY_POMO "pomo_cnt"

/* ── Alert melody ─────────────────────────────────────────── */
static const tone_note_t s_alert[] = {
    { 880, 120 }, { 0, 60 },
    { 880, 120 }, { 0, 60 },
    { 1047, 200 },
};
#define ALERT_LEN  (sizeof(s_alert) / sizeof(s_alert[0]))

/* ── State ────────────────────────────────────────────────── */
typedef enum { PM_IDLE, PM_RUNNING, PM_PAUSED } pm_run_t;
typedef enum { PM_WORK, PM_BREAK }              pm_phase_t;

static bool        s_active  = false;
static lv_obj_t   *s_overlay = NULL;

static pm_run_t    s_run     = PM_IDLE;
static pm_phase_t  s_phase   = PM_WORK;
static int         s_remain;          /* seconds remaining */
static int         s_sessions;        /* completed work sessions */
static int64_t     s_last_us;         /* last tick timestamp */

/* ── LVGL widgets ─────────────────────────────────────────── */
static lv_obj_t   *s_time_lbl   = NULL;
static lv_obj_t   *s_phase_lbl  = NULL;
static lv_obj_t   *s_sess_lbl   = NULL;
static lv_obj_t   *s_start_btn  = NULL;
static lv_obj_t   *s_start_lbl  = NULL;
static lv_obj_t   *s_reset_btn  = NULL;
static lv_obj_t   *s_prog_bar   = NULL;

/* ── Helpers ──────────────────────────────────────────────── */
static int phase_total(void)
{
    return (s_phase == PM_WORK) ? WORK_SECS : BREAK_SECS;
}

static void load_sessions(void)
{
    s_sessions = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_POMO, &v) == ESP_OK)
            s_sessions = v;
        nvs_close(h);
    }
}

static void save_sessions(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_POMO, (uint8_t)(s_sessions & 0xFF));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void update_time_label(void)
{
    if (!s_time_lbl) return;
    int m = s_remain / 60;
    int s = s_remain % 60;
    lv_label_set_text_fmt(s_time_lbl, "%02d:%02d", m, s);
}

static void update_phase_label(void)
{
    if (s_phase_lbl)
        lv_label_set_text(s_phase_lbl,
            (s_phase == PM_WORK) ? "WORK" : "BREAK");
}

static void update_sess_label(void)
{
    if (s_sess_lbl)
        lv_label_set_text_fmt(s_sess_lbl, "Sessions: %d", s_sessions);
}

static void update_button_text(void)
{
    if (!s_start_lbl) return;
    switch (s_run) {
        case PM_IDLE:    lv_label_set_text(s_start_lbl, LV_SYMBOL_PLAY " Start"); break;
        case PM_RUNNING: lv_label_set_text(s_start_lbl, LV_SYMBOL_PAUSE " Pause"); break;
        case PM_PAUSED:  lv_label_set_text(s_start_lbl, LV_SYMBOL_PLAY " Resume"); break;
    }
}

static void update_progress(void)
{
    if (!s_prog_bar) return;
    int total = phase_total();
    int pct = (total > 0) ? ((total - s_remain) * 100 / total) : 0;
    lv_bar_set_value(s_prog_bar, pct, LV_ANIM_ON);

    /* Color: work=tomato red, break=green */
    uint32_t col = (s_phase == PM_WORK) ? 0xE53935 : 0x43A047;
    lv_obj_set_style_bg_color(s_prog_bar, lv_color_hex(col),
                               LV_PART_INDICATOR);
}

/* ── Callbacks ────────────────────────────────────────────── */
static void start_cb(lv_event_t *e)
{
    (void)e;
    if (s_run == PM_RUNNING) {
        s_run = PM_PAUSED;
    } else {
        if (s_run == PM_IDLE) {
            s_remain = phase_total();
            update_time_label();
            update_progress();
        }
        s_run = PM_RUNNING;
        s_last_us = esp_timer_get_time();
    }
    update_button_text();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    s_run   = PM_IDLE;
    s_phase = PM_WORK;
    s_remain = WORK_SECS;
    update_time_label();
    update_phase_label();
    update_button_text();
    update_progress();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    pomodoro_close();
}

/* ── Build UI ─────────────────────────────────────────────── */
static void build_ui(void)
{
    /* Background */
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0A0A14), 0);

    /* ── Back button ─────────────────────────────────── */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 60, 28);
    lv_obj_set_pos(back, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* ── Title ───────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, LV_SYMBOL_BELL "  Pomodoro");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* ── Phase label (WORK / BREAK) ──────────────────── */
    s_phase_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_phase_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_phase_lbl, lv_color_hex(0xE53935), 0);
    lv_obj_set_pos(s_phase_lbl, 120, 42);
    update_phase_label();

    /* ── Big countdown timer ─────────────────────────── */
    s_time_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(s_time_lbl, 210, 42);
    update_time_label();

    /* ── Session counter ─────────────────────────────── */
    s_sess_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_sess_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sess_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(s_sess_lbl, 120, 100);
    update_sess_label();

    /* ── Progress bar ────────────────────────────────── */
    s_prog_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_prog_bar, 280, 10);
    lv_obj_set_pos(s_prog_bar, 120, 126);
    lv_obj_set_style_bg_color(s_prog_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_prog_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_prog_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_prog_bar, 5, LV_PART_INDICATOR);
    lv_bar_set_range(s_prog_bar, 0, 100);
    lv_bar_set_value(s_prog_bar, 0, LV_ANIM_OFF);
    update_progress();

    /* ── Start/Pause button ──────────────────────────── */
    s_start_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_start_btn, 120, 50);
    lv_obj_set_pos(s_start_btn, 440, 40);
    lv_obj_set_style_bg_color(s_start_btn, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(s_start_btn, 12, 0);
    lv_obj_add_event_cb(s_start_btn, start_cb, LV_EVENT_CLICKED, NULL);

    s_start_lbl = lv_label_create(s_start_btn);
    lv_obj_set_style_text_font(s_start_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_start_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_start_lbl);
    update_button_text();

    /* ── Reset button ────────────────────────────────── */
    s_reset_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_reset_btn, 120, 50);
    lv_obj_set_pos(s_reset_btn, 440, 100);
    lv_obj_set_style_bg_color(s_reset_btn, lv_color_hex(0x8B0000), 0);
    lv_obj_set_style_radius(s_reset_btn, 12, 0);
    lv_obj_add_event_cb(s_reset_btn, reset_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rst_lbl = lv_label_create(s_reset_btn);
    lv_label_set_text(rst_lbl, LV_SYMBOL_REFRESH " Reset");
    lv_obj_set_style_text_font(rst_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(rst_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(rst_lbl);

    /* ── Tomato emoji decoration (left side) ─────────── */
    lv_obj_t *deco = lv_label_create(s_overlay);
    lv_label_set_text(deco, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(deco, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(deco, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_text_opa(deco, LV_OPA_30, 0);
    lv_obj_set_pos(deco, 30, 60);
}

/* ── Period complete — switch phase + alert ───────────────── */
static void on_period_complete(void)
{
    /* Play alert sound */
    play_melody_async(s_alert, ALERT_LEN, g_volume);

    if (s_phase == PM_WORK) {
        s_sessions++;
        save_sessions();
        update_sess_label();
        /* Switch to break */
        s_phase = PM_BREAK;
        s_remain = BREAK_SECS;
        ESP_LOGI(TAG, "Work done! Session #%d. Break time.", s_sessions);
    } else {
        /* Switch to work */
        s_phase = PM_WORK;
        s_remain = WORK_SECS;
        ESP_LOGI(TAG, "Break done! Back to work.");
    }

    /* Update phase colors */
    if (s_phase_lbl) {
        uint32_t col = (s_phase == PM_WORK) ? 0xE53935 : 0x43A047;
        lv_obj_set_style_text_color(s_phase_lbl, lv_color_hex(col), 0);
    }
    if (s_start_btn) {
        uint32_t col = (s_phase == PM_WORK) ? 0x1B5E20 : 0x00695C;
        lv_obj_set_style_bg_color(s_start_btn, lv_color_hex(col), 0);
    }

    update_phase_label();
    update_time_label();
    update_progress();

    /* Auto-start next period */
    s_run = PM_RUNNING;
    s_last_us = esp_timer_get_time();
    update_button_text();
}

/* ================================================================
 *  Public API
 * ================================================================ */
void pomodoro_open(lv_obj_t *parent)
{
    if (s_active) return;

    load_sessions();

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, GW, GH);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_active = true;
    s_run    = PM_IDLE;
    s_phase  = PM_WORK;
    s_remain = WORK_SECS;

    build_ui();
    ESP_LOGI(TAG, "Pomodoro opened (sessions: %d)", s_sessions);
}

void pomodoro_close(void)
{
    if (!s_active) return;

    s_time_lbl  = NULL;
    s_phase_lbl = NULL;
    s_sess_lbl  = NULL;
    s_start_btn = NULL;
    s_start_lbl = NULL;
    s_reset_btn = NULL;
    s_prog_bar  = NULL;

    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_run    = PM_IDLE;
    ESP_LOGI(TAG, "Pomodoro closed");
}

bool pomodoro_is_active(void)
{
    return s_active;
}

void pomodoro_update(void)
{
    if (!s_active || s_run != PM_RUNNING) return;

    int64_t now = esp_timer_get_time();
    int64_t elapsed_us = now - s_last_us;

    /* Only decrement every full second */
    if (elapsed_us >= 1000000) {
        int secs = (int)(elapsed_us / 1000000);
        s_last_us += (int64_t)secs * 1000000;
        s_remain -= secs;

        if (s_remain <= 0) {
            s_remain = 0;
            update_time_label();
            update_progress();
            on_period_complete();
            return;
        }

        update_time_label();
        update_progress();
    }
}
