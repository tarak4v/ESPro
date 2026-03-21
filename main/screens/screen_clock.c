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
#include "app_manager.h"
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
static lv_obj_t *mode_dot[4];
static lv_obj_t *wifi_icon      = NULL;
static lv_obj_t *bt_icon        = NULL;
static lv_obj_t *weather_temp   = NULL;
static lv_obj_t *weather_desc   = NULL;
static lv_obj_t *weather_loc    = NULL;

/* Flip clock digit labels (4 digit cards: HH MM) */
static lv_obj_t *flip_digit[4]  = {NULL};
static lv_obj_t *flip_colon     = NULL;
static char      flip_prev[4]   = {'?','?','?','?'}; /* previous digits for anim */

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
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    /* Frosted-glass panel */
    lv_style_init(&style_glass);
    lv_style_set_bg_color(&style_glass, lv_color_hex(th_card));
    lv_style_set_bg_opa(&style_glass, g_theme_dark ? LV_OPA_80 : LV_OPA_COVER);
    lv_style_set_border_color(&style_glass,
                              lv_color_hex(g_theme_dark ? g_face.glass_border_d
                                                        : g_face.glass_border_l));
    lv_style_set_border_width(&style_glass, 1);
    lv_style_set_border_opa(&style_glass, LV_OPA_60);
    lv_style_set_radius(&style_glass, 16);
    lv_style_set_shadow_color(&style_glass,
                              lv_color_hex(g_theme_dark ? 0x000000 : 0x888888));
    lv_style_set_shadow_width(&style_glass, g_theme_dark ? 20 : 8);
    lv_style_set_shadow_opa(&style_glass, LV_OPA_40);
    lv_style_set_pad_all(&style_glass, 12);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active,
                          lv_color_hex(g_theme_dark ? g_face.accent_d
                                                    : g_face.accent_l));
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

/* ── Flip-card appear animation (opacity flash) ──────────── */
static void flip_anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void flip_card_animate(lv_obj_t *card)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, flip_anim_opa_cb);
    lv_anim_set_values(&a, 60, 255);
    lv_anim_set_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ── Helper: create one flip-digit card ───────────────────── */
static lv_obj_t *create_flip_card(lv_obj_t *parent, int x, int y)
{
    uint32_t card_bg = g_theme_dark ? 0x1C1C2E : 0xEEEEF2;
    uint32_t line_c  = g_theme_dark ? 0x333344 : 0xCCCCDD;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 55, 80);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(line_c), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Horizontal divider line at centre */
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 55, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(line_c), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);

    /* Digit label */
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "0");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th_text), 0);
    lv_obj_center(lbl);

    return lbl;  /* return the LABEL for updating */
}

/* ── Create ───────────────────────────────────────────────── */
void screen_clock_create(void)
{
    init_styles();

    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Theme-aware text colours — from active watch face */
    uint32_t c_time   = g_theme_dark ? g_face.time_d   : g_face.time_l;
    uint32_t c_ampm   = g_theme_dark ? g_face.ampm_d   : g_face.ampm_l;
    uint32_t c_date   = g_theme_dark ? g_face.date_d   : g_face.date_l;
    uint32_t c_wkday  = g_theme_dark ? g_face.weekday_d: g_face.weekday_l;
    uint32_t c_wdesc  = g_theme_dark ? g_face.wdesc_d  : g_face.wdesc_l;
    uint32_t c_wloc   = g_theme_dark ? g_face.wloc_d   : g_face.wloc_l;
    uint32_t c_ico_df = g_theme_dark ? g_face.icon_d   : g_face.icon_l;

    /* ── Status bar (WiFi + BT icons, top-left) ──── */
    wifi_icon = lv_label_create(scr);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(c_ico_df), 0);
    lv_obj_set_pos(wifi_icon, 10, 4);

    bt_icon = lv_label_create(scr);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bt_icon, lv_color_hex(c_ico_df), 0);
    lv_obj_set_pos(bt_icon, 28, 4);

    /* ════════════ LEFT GLASS PANEL — Clock & Date ═══════════ */
    lv_obj_t *left_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(left_panel);
    lv_obj_add_style(left_panel, &style_glass, 0);
    lv_obj_set_size(left_panel, 300, 140);
    lv_obj_set_pos(left_panel, 14, 18);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    if (g_clock_flip) {
        /* ── FLIP CLOCK: 4 individual digit cards ──── */
        /*  Layout inside 300×140 panel (12px pad → 276×116 usable)
         *  Card: 55×80 each.  HH[2px]HH : MM[2px]MM
         *  Total: 55+2+55 + 14 + 55+2+55 = 238 → starts at (276-238)/2 = 19 */
        int base_x = 19;
        int base_y = 4;  /* within panel padding */
        flip_digit[0] = create_flip_card(left_panel, base_x,      base_y);
        flip_digit[1] = create_flip_card(left_panel, base_x + 57, base_y);

        /* Colon */
        flip_colon = lv_label_create(left_panel);
        lv_label_set_text(flip_colon, ":");
        lv_obj_set_style_text_font(flip_colon, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(flip_colon, lv_color_hex(c_time), 0);
        lv_obj_set_pos(flip_colon, base_x + 117, base_y + 14);

        flip_digit[2] = create_flip_card(left_panel, base_x + 138, base_y);
        flip_digit[3] = create_flip_card(left_panel, base_x + 195, base_y);

        /* AM/PM (right of last card) */
        ampm_label = lv_label_create(left_panel);
        lv_label_set_text(ampm_label, "");
        lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ampm_label, lv_color_hex(c_ampm), 0);
        lv_obj_set_pos(ampm_label, base_x + 202, base_y + 82);

        /* Date below flip cards */
        date_label = lv_label_create(left_panel);
        lv_label_set_text(date_label, "01-01-2025");
        lv_obj_set_style_text_font(date_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(date_label, lv_color_hex(c_date), 0);
        lv_obj_align(date_label, LV_ALIGN_BOTTOM_MID, -20, -2);

        /* Weekday */
        weekday_label = lv_label_create(left_panel);
        lv_label_set_text(weekday_label, "SAT");
        lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(weekday_label, lv_color_hex(c_wkday), 0);
        lv_obj_align(weekday_label, LV_ALIGN_BOTTOM_MID, 60, -2);

        time_label = NULL;   /* not used in flip mode */
    } else {
        /* ── DIGITAL CLOCK (original style) ──── */
        for (int i = 0; i < 4; i++) flip_digit[i] = NULL;
        flip_colon = NULL;

        /* Time (large) */
        time_label = lv_label_create(left_panel);
        lv_label_set_text(time_label, "12:00");
        lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(time_label, lv_color_hex(c_time), 0);
        lv_obj_set_style_text_letter_space(time_label, 4, 0);
        lv_obj_align(time_label, LV_ALIGN_CENTER, -14, -18);

        /* AM/PM */
        ampm_label = lv_label_create(left_panel);
        lv_label_set_text(ampm_label, "AM");
        lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ampm_label, lv_color_hex(c_ampm), 0);
        lv_obj_align_to(ampm_label, time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);

        /* Date */
        date_label = lv_label_create(left_panel);
        lv_label_set_text(date_label, "01-01-2025");
        lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(date_label, lv_color_hex(c_date), 0);
        lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 26);

        /* Weekday */
        weekday_label = lv_label_create(left_panel);
        lv_label_set_text(weekday_label, "SAT");
        lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(weekday_label, lv_color_hex(c_wkday), 0);
        lv_obj_align(weekday_label, LV_ALIGN_CENTER, 0, 44);
    }

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
    lv_obj_set_style_text_color(weather_temp,
        lv_color_hex(g_theme_dark ? g_face.wtemp_d : g_face.wtemp_l), 0);
    lv_obj_align(weather_temp, LV_ALIGN_CENTER, 0, -22);

    /* Weather description */
    weather_desc = lv_label_create(right_panel);
    lv_label_set_text(weather_desc, "---");
    lv_obj_set_style_text_font(weather_desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_desc, lv_color_hex(c_wdesc), 0);
    lv_obj_align(weather_desc, LV_ALIGN_CENTER, 0, 16);

    /* Location */
    weather_loc = lv_label_create(right_panel);
    lv_label_set_text(weather_loc, LV_SYMBOL_GPS " " WEATHER_CITY);
    lv_obj_set_style_text_font(weather_loc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(weather_loc, lv_color_hex(c_wloc), 0);
    lv_obj_align(weather_loc, LV_ALIGN_CENTER, 0, 38);

    /* ── Page indicator dots (3) ──── */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 50, 10);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 0)
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        else
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
    }

    g_pending_scr = scr;
    screen_clock_update();
    ESP_LOGI(TAG, "Clock screen created (%s, %s theme)",
             g_clock_flip ? "flip" : "digital",
             g_theme_dark ? "dark" : "light");
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
        for (int i = 0; i < 4; i++) mode_dot[i] = NULL;
        for (int i = 0; i < 4; i++) flip_digit[i] = NULL;
        flip_colon    = NULL;
        memset(flip_prev, '?', sizeof(flip_prev));
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
    if (flip_digit[0]) {
        /* Flip clock: update individual digit labels + animate on change */
        char d[2] = {0, 0};
        for (int i = 0; i < 4; i++) {
            int bi = (i < 2) ? i : i + 1;  /* skip ':' at index 2 */
            d[0] = buf[bi];
            if (d[0] != flip_prev[i]) {
                lv_label_set_text(flip_digit[i], d);
                /* Animate the card (parent of the label) */
                lv_obj_t *card = lv_obj_get_parent(flip_digit[i]);
                if (card) flip_card_animate(card);
                flip_prev[i] = d[0];
            }
        }
    } else if (time_label) {
        lv_label_set_text(time_label, buf);
    }

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
