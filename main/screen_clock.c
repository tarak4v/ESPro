/**
 * @file screen_clock.c
 * @brief Clock screen — Glassy Tesla-inspired theme.
 *
 * Two frosted-glass panels on a dark background:
 *   Left  panel — digital clock (12h/24h) + date
 *   Right panel — temperature, weather description, location
 *
 * Designed for 640×172 landscape AMOLED.
 */

#include "screen_clock.h"
#include "screen_settings.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "wifi_time.h"
#include "weather.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "clock";

/* ── UI handles ───────────────────────────────────────────── */
static lv_obj_t *scr            = NULL;
static lv_obj_t *time_label     = NULL;
static lv_obj_t *ampm_label     = NULL;
static lv_obj_t *date_label     = NULL;
static lv_obj_t *weekday_label  = NULL;
static lv_obj_t *mode_dot[2];
static lv_obj_t *wifi_icon      = NULL;
static lv_obj_t *bt_icon        = NULL;
static lv_obj_t *weather_temp   = NULL;
static lv_obj_t *weather_desc   = NULL;
static lv_obj_t *weather_loc    = NULL;

static const char *weekday_names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

/* ── BCD helpers ──────────────────────────────────────────── */
static inline uint8_t bcd2dec(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static inline uint8_t dec2bcd(uint8_t dec)
{
    return ((dec / 10) << 4) | (dec % 10);
}

/* ── RTC read ─────────────────────────────────────────────── */
typedef struct {
    uint8_t sec, min, hour, day, weekday, month;
    uint16_t year;
    bool valid;
} rtc_time_t;

static rtc_time_t read_rtc(void)
{
    rtc_time_t t = {0};
    uint8_t buf[7];

    if (i2c_read_buff(rtc_dev_handle, PCF85063_REG_SECONDS, buf, 7) != 0) {
        t.valid = false;
        return t;
    }

    t.sec     = bcd2dec(buf[0] & 0x7F);
    t.min     = bcd2dec(buf[1] & 0x7F);
    t.hour    = bcd2dec(buf[2] & 0x3F);
    t.day     = bcd2dec(buf[3] & 0x3F);
    t.weekday = buf[4] & 0x07;
    t.month   = bcd2dec(buf[5] & 0x1F);
    t.year    = 2000 + bcd2dec(buf[6]);
    t.valid   = true;
    return t;
}

/* ── RTC initialisation ───────────────────────────────────── */
void pcf85063_init(void)
{
    uint8_t ctrl1 = 0x00;
    i2c_writr_buff(rtc_dev_handle, PCF85063_REG_CTRL1, &ctrl1, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t buf[7];
    if (i2c_read_buff(rtc_dev_handle, PCF85063_REG_SECONDS, buf, 7) == 0) {
        uint8_t year = bcd2dec(buf[6]);
        if (year == 0) {
            uint8_t t[7];
            t[0] = dec2bcd(0);
            t[1] = dec2bcd(0);
            t[2] = dec2bcd(12);
            t[3] = dec2bcd(8);
            t[4] = 0;
            t[5] = dec2bcd(3);
            t[6] = dec2bcd(26);
            i2c_writr_buff(rtc_dev_handle, PCF85063_REG_SECONDS, t, 7);
            ESP_LOGI(TAG, "RTC was unset — wrote default 2026-03-08 12:00:00 IST");
        } else {
            ESP_LOGI(TAG, "RTC already set (year=20%02d)", year);
        }
    } else {
        ESP_LOGW(TAG, "Failed to read RTC during init");
    }
}

/* ── Styles ───────────────────────────────────────────────── */
static lv_style_t style_bg, style_glass;
static lv_style_t style_dot_active, style_dot_inactive;

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(0x0A0A14));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    /* Frosted-glass panel */
    lv_style_init(&style_glass);
    lv_style_set_bg_color(&style_glass, lv_color_hex(0x1A1A2E));
    lv_style_set_bg_opa(&style_glass, LV_OPA_80);
    lv_style_set_border_color(&style_glass, lv_color_hex(0x333355));
    lv_style_set_border_width(&style_glass, 1);
    lv_style_set_border_opa(&style_glass, LV_OPA_60);
    lv_style_set_radius(&style_glass, 16);
    lv_style_set_shadow_color(&style_glass, lv_color_hex(0x000000));
    lv_style_set_shadow_width(&style_glass, 20);
    lv_style_set_shadow_opa(&style_glass, LV_OPA_40);
    lv_style_set_pad_all(&style_glass, 12);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(0x00DDFF));
    lv_style_set_bg_opa(&style_dot_active, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_active, 8);
    lv_style_set_height(&style_dot_active, 8);

    lv_style_init(&style_dot_inactive);
    lv_style_set_bg_color(&style_dot_inactive, lv_color_hex(0x333344));
    lv_style_set_bg_opa(&style_dot_inactive, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_inactive, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_inactive, 6);
    lv_style_set_height(&style_dot_inactive, 6);
}

/* ── Create ───────────────────────────────────────────────── */
void screen_clock_create(void)
{
    init_styles();

    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Status bar (WiFi + BT icons, top-left) ──── */
    wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x444466), 0);
    lv_obj_set_pos(wifi_icon, 10, 4);

    bt_icon = lv_label_create(scr);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bt_icon, lv_color_hex(0x444466), 0);
    lv_obj_set_pos(bt_icon, 28, 4);

    /* ════════════ LEFT GLASS PANEL — Clock & Date ═══════════ */
    lv_obj_t *left_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(left_panel);
    lv_obj_add_style(left_panel, &style_glass, 0);
    lv_obj_set_size(left_panel, 300, 140);
    lv_obj_set_pos(left_panel, 14, 18);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Time (large) */
    time_label = lv_label_create(left_panel);
    lv_label_set_text(time_label, "12:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xEEEEFF), 0);
    lv_obj_set_style_text_letter_space(time_label, 4, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, -14, -18);

    /* AM/PM */
    ampm_label = lv_label_create(left_panel);
    lv_label_set_text(ampm_label, "AM");
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0x6688AA), 0);
    lv_obj_align_to(ampm_label, time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);

    /* Date */
    date_label = lv_label_create(left_panel);
    lv_label_set_text(date_label, "01-01-2025");
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x7788AA), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 26);

    /* Weekday */
    weekday_label = lv_label_create(left_panel);
    lv_label_set_text(weekday_label, "SAT");
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weekday_label, lv_color_hex(0x00BBDD), 0);
    lv_obj_align(weekday_label, LV_ALIGN_CENTER, 0, 44);

    /* ════════════ RIGHT GLASS PANEL — Weather ════════════════ */
    lv_obj_t *right_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(right_panel);
    lv_obj_add_style(right_panel, &style_glass, 0);
    lv_obj_set_size(right_panel, 296, 140);
    lv_obj_set_pos(right_panel, 326, 18);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Temperature (large) */
    weather_temp = lv_label_create(right_panel);
    lv_label_set_text(weather_temp, "--\xC2\xB0""C");
    lv_obj_set_style_text_font(weather_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(weather_temp, lv_color_hex(0xFFCC44), 0);
    lv_obj_align(weather_temp, LV_ALIGN_CENTER, 0, -22);

    /* Weather description */
    weather_desc = lv_label_create(right_panel);
    lv_label_set_text(weather_desc, "---");
    lv_obj_set_style_text_font(weather_desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_desc, lv_color_hex(0x8899BB), 0);
    lv_obj_align(weather_desc, LV_ALIGN_CENTER, 0, 16);

    /* Location */
    weather_loc = lv_label_create(right_panel);
    lv_label_set_text(weather_loc, LV_SYMBOL_GPS " " WEATHER_CITY);
    lv_obj_set_style_text_font(weather_loc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(weather_loc, lv_color_hex(0x556688), 0);
    lv_obj_align(weather_loc, LV_ALIGN_CENTER, 0, 38);

    /* ── Page indicator dots (2) ──── */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 40, 10);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 0)
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        else
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
    }

    lv_disp_load_scr(scr);
    screen_clock_update();
    ESP_LOGI(TAG, "Clock screen created (glassy theme)");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_clock_destroy(void)
{
    if (scr) {
        lv_obj_del(scr);
        scr = NULL;
        time_label    = NULL;
        ampm_label    = NULL;
        date_label    = NULL;
        weekday_label = NULL;
        for (int i = 0; i < 2; i++) mode_dot[i] = NULL;
        wifi_icon     = NULL;
        bt_icon       = NULL;
        weather_temp  = NULL;
        weather_desc  = NULL;
        weather_loc   = NULL;
    }
}

/* ── Periodic update (called from app_manager_update) ─────── */
void screen_clock_update(void)
{
    if (scr == NULL) return;

    rtc_time_t t = read_rtc();
    if (!t.valid) return;

    /* 12h / 24h mode */
    char buf[8];
    if (g_clock_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
        if (ampm_label) lv_label_set_text(ampm_label, "");
    } else {
        uint8_t h12 = t.hour % 12;
        if (h12 == 0) h12 = 12;
        const char *ampm = (t.hour < 12) ? "AM" : "PM";
        snprintf(buf, sizeof(buf), "%2d:%02d", h12, t.min);
        if (ampm_label) lv_label_set_text(ampm_label, ampm);
    }
    if (time_label)
        lv_label_set_text(time_label, buf);

    if (date_label)
        lv_label_set_text_fmt(date_label, "%02d-%02d-%04d",
                              t.day, t.month, t.year);
    if (weekday_label)
        lv_label_set_text(weekday_label, weekday_names[t.weekday % 7]);

    /* WiFi status icon colour */
    if (wifi_icon) {
        lv_color_t c = wifi_is_connected()
            ? lv_color_hex(0x00FF88)
            : lv_color_hex(0xFF4444);
        lv_obj_set_style_text_color(wifi_icon, c, 0);
    }

    /* Bluetooth icon (grey — no active BT yet) */
    if (bt_icon)
        lv_obj_set_style_text_color(bt_icon, lv_color_hex(0x666666), 0);

    /* Weather (same font as clock) */
    weather_data_t w = weather_get();
    if (w.valid && weather_temp && weather_desc) {
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "%.0f\xC2\xB0""C", w.temp);
        lv_label_set_text(weather_temp, wbuf);
        lv_label_set_text(weather_desc, w.description);
    }
}
