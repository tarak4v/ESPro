/**
 * @file screen_clock.c
 * @brief Clock screen — 12-hour digital clock with AM/PM, date/weekday,
 *        WiFi/BT status icons, and weather display.
 *
 * Time font: montserrat_48.  Weather temp uses the same font.
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
static lv_obj_t *digit_labels[5];          /* "HH:MM" (5 chars) */
static lv_obj_t *ampm_label     = NULL;
static lv_obj_t *date_label     = NULL;
static lv_obj_t *weekday_label  = NULL;
static lv_obj_t *mode_dot[3];             /* 3 page indicator dots */
static lv_obj_t *wifi_icon      = NULL;
static lv_obj_t *bt_icon        = NULL;
static lv_obj_t *weather_temp   = NULL;
static lv_obj_t *weather_desc   = NULL;

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
static lv_style_t style_bg, style_digit, style_colon, style_ampm;
static lv_style_t style_date;
static lv_style_t style_dot_active, style_dot_inactive;

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(0x000000));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_digit);
    lv_style_set_text_color(&style_digit, lv_color_hex(0x00FF88));
    lv_style_set_text_font(&style_digit, &lv_font_montserrat_48);

    lv_style_init(&style_colon);
    lv_style_set_text_color(&style_colon, lv_color_hex(0x00CC66));
    lv_style_set_text_font(&style_colon, &lv_font_montserrat_48);

    lv_style_init(&style_ampm);
    lv_style_set_text_color(&style_ampm, lv_color_hex(0x00CC66));
    lv_style_set_text_font(&style_ampm, &lv_font_montserrat_28);

    lv_style_init(&style_date);
    lv_style_set_text_color(&style_date, lv_color_hex(0x888888));
    lv_style_set_text_font(&style_date, &lv_font_montserrat_16);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(0x00FF88));
    lv_style_set_bg_opa(&style_dot_active, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_active, 8);
    lv_style_set_height(&style_dot_active, 8);

    lv_style_init(&style_dot_inactive);
    lv_style_set_bg_color(&style_dot_inactive, lv_color_hex(0x333333));
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

    /* ── WiFi & BT icons (top-left) ──── */
    wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x666666), 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 10, 4);

    bt_icon = lv_label_create(scr);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bt_icon, lv_color_hex(0x666666), 0);
    lv_obj_align(bt_icon, LV_ALIGN_TOP_LEFT, 30, 4);

    /* ── Time digits container: "HH:MM" ──── */
    lv_obj_t *time_row = lv_obj_create(scr);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, 300, 60);
    lv_obj_align(time_row, LV_ALIGN_LEFT_MID, 60, -25);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

    const char *init_str = "12:00";
    for (int i = 0; i < 5; i++) {
        digit_labels[i] = lv_label_create(time_row);
        lv_label_set_text_fmt(digit_labels[i], "%c", init_str[i]);
        if (init_str[i] == ':')
            lv_obj_add_style(digit_labels[i], &style_colon, 0);
        else
            lv_obj_add_style(digit_labels[i], &style_digit, 0);
    }

    /* ── AM/PM label (next to time) ──── */
    ampm_label = lv_label_create(scr);
    lv_label_set_text(ampm_label, "AM");
    lv_obj_add_style(ampm_label, &style_ampm, 0);
    lv_obj_align_to(ampm_label, time_row, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -4);

    /* ── Date + weekday row ──── */
    lv_obj_t *info_row = lv_obj_create(scr);
    lv_obj_remove_style_all(info_row);
    lv_obj_set_size(info_row, 350, 24);
    lv_obj_align(info_row, LV_ALIGN_LEFT_MID, 60, 22);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(info_row, 20, 0);
    lv_obj_clear_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);

    date_label = lv_label_create(info_row);
    lv_label_set_text(date_label, "01-01-2025");
    lv_obj_add_style(date_label, &style_date, 0);

    weekday_label = lv_label_create(info_row);
    lv_label_set_text(weekday_label, "SAT");
    lv_obj_add_style(weekday_label, &style_date, 0);

    /* ── Weather display (right side, same font as clock) ──── */
    weather_temp = lv_label_create(scr);
    lv_label_set_text(weather_temp, "--");
    lv_obj_set_style_text_font(weather_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(weather_temp, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(weather_temp, LV_ALIGN_RIGHT_MID, -20, -20);

    weather_desc = lv_label_create(scr);
    lv_label_set_text(weather_desc, "---");
    lv_obj_set_style_text_font(weather_desc, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(weather_desc, lv_color_hex(0x888888), 0);
    lv_obj_align(weather_desc, LV_ALIGN_RIGHT_MID, -20, 20);

    /* ── Page indicator dots (3) ──── */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 60, 12);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 0)
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        else
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
    }

    lv_disp_load_scr(scr);
    screen_clock_update();
    ESP_LOGI(TAG, "Clock screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_clock_destroy(void)
{
    if (scr) {
        lv_obj_del(scr);
        scr = NULL;
        for (int i = 0; i < 5; i++) digit_labels[i] = NULL;
        ampm_label    = NULL;
        date_label    = NULL;
        weekday_label = NULL;
        for (int i = 0; i < 3; i++) mode_dot[i] = NULL;
        wifi_icon     = NULL;
        bt_icon       = NULL;
        weather_temp  = NULL;
        weather_desc  = NULL;
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
        snprintf(buf, sizeof(buf), "%2d:%02d", t.hour, t.min);
        if (ampm_label) lv_label_set_text(ampm_label, "");
    } else {
        uint8_t h12 = t.hour % 12;
        if (h12 == 0) h12 = 12;
        const char *ampm = (t.hour < 12) ? "AM" : "PM";
        snprintf(buf, sizeof(buf), "%2d:%02d", h12, t.min);
        if (ampm_label) lv_label_set_text(ampm_label, ampm);
    }
    for (int i = 0; i < 5; i++) {
        if (digit_labels[i])
            lv_label_set_text_fmt(digit_labels[i], "%c", buf[i]);
    }

    lv_label_set_text_fmt(date_label, "%02d-%02d-%04d",
                          t.day, t.month, t.year);
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
