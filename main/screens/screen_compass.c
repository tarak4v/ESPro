/**
 * @file screen_compass.c
 * @brief IMU / Compass screen — shows accelerometer + gyroscope data
 *        from the QMI8658 sensor in real-time.
 *
 * MeowKit-inspired: similar to their air_mouse / IMU readout mode.
 * Designed for 640×172 landscape layout.
 */

#include "screen_compass.h"
#include "screen_settings.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "compass";

static lv_obj_t *scr  = NULL;
static lv_obj_t *lbl_accel = NULL;
static lv_obj_t *lbl_gyro  = NULL;
static lv_obj_t *lbl_pitch = NULL;
static lv_obj_t *lbl_roll  = NULL;
static lv_obj_t *horizon_line = NULL;
static lv_obj_t *mode_dot[4];

static bool imu_initialized = false;

/* ── QMI8658 basic init ───────────────────────────────────── */
static void qmi8658_init(void)
{
    uint8_t who_am_i = 0;
    i2c_read_buff(imu_dev_handle, QMI8658_REG_WHO_AM_I, &who_am_i, 1);
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I: 0x%02X", who_am_i);

    /* CTRL1: enable sensors address auto-increment */
    uint8_t val = 0x40;
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL1, &val, 1);

    /* CTRL2: Accel ODR 250Hz, ±8g */
    val = 0x25;
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL2, &val, 1);

    /* CTRL3: Gyro ODR 250Hz, ±512 dps */
    val = 0x65;
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL3, &val, 1);

    /* CTRL7: Enable accel + gyro */
    val = 0x03;
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL7, &val, 1);

    imu_initialized = true;
}

/* ── Read accel + gyro raw data ───────────────────────────── */
typedef struct {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} imu_raw_t;

static imu_raw_t read_imu(void)
{
    imu_raw_t d = {0};
    uint8_t buf[12];

    /* Read 6 bytes of accel data */
    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) == 0) {
        d.ax = (int16_t)((buf[1] << 8) | buf[0]);
        d.ay = (int16_t)((buf[3] << 8) | buf[2]);
        d.az = (int16_t)((buf[5] << 8) | buf[4]);
    }

    /* Read 6 bytes of gyro data */
    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_GX_L, buf, 6) == 0) {
        d.gx = (int16_t)((buf[1] << 8) | buf[0]);
        d.gy = (int16_t)((buf[3] << 8) | buf[2]);
        d.gz = (int16_t)((buf[5] << 8) | buf[4]);
    }
    return d;
}

/* ── Styles ───────────────────────────────────────────────── */
static lv_style_t style_bg;
static lv_style_t style_data;
static lv_style_t style_angle;
static lv_style_t style_dot_active;
static lv_style_t style_dot_inactive;

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_data);
    lv_style_set_text_color(&style_data, lv_color_hex(g_theme_dark ? 0x66AAFF : 0x2266CC));
    lv_style_set_text_font(&style_data, &lv_font_montserrat_12);

    lv_style_init(&style_angle);
    lv_style_set_text_color(&style_angle, lv_color_hex(g_theme_dark ? 0xFFCC44 : 0xCC8800));
    lv_style_set_text_font(&style_angle, &lv_font_montserrat_20);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(g_theme_dark ? 0x66AAFF : 0x2266CC));
    lv_style_set_bg_opa(&style_dot_active, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_active, 8);
    lv_style_set_height(&style_dot_active, 8);

    lv_style_init(&style_dot_inactive);
    lv_style_set_bg_color(&style_dot_inactive, lv_color_hex(th_btn));
    lv_style_set_bg_opa(&style_dot_inactive, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_inactive, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_inactive, 6);
    lv_style_set_height(&style_dot_inactive, 6);
}

/* ── Create ───────────────────────────────────────────────── */
void screen_compass_create(void)
{
    init_styles();

    if (!imu_initialized) {
        qmi8658_init();
    }

    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_GPS " IMU");
    lv_obj_set_style_text_color(title, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* ── Left column: raw data ──── */
    lv_obj_t *col_left = lv_obj_create(scr);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_size(col_left, 300, 100);
    lv_obj_align(col_left, LV_ALIGN_LEFT_MID, 20, 8);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

    lbl_accel = lv_label_create(col_left);
    lv_obj_add_style(lbl_accel, &style_data, 0);
    lv_label_set_text(lbl_accel, "Accel: --- --- ---");
    lv_obj_align(lbl_accel, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_gyro = lv_label_create(col_left);
    lv_obj_add_style(lbl_gyro, &style_data, 0);
    lv_label_set_text(lbl_gyro, "Gyro:  --- --- ---");
    lv_obj_align(lbl_gyro, LV_ALIGN_TOP_LEFT, 0, 24);

    /* ── Right column: computed angles ──── */
    lv_obj_t *col_right = lv_obj_create(scr);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_size(col_right, 260, 100);
    lv_obj_align(col_right, LV_ALIGN_RIGHT_MID, -20, 8);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

    lbl_pitch = lv_label_create(col_right);
    lv_obj_add_style(lbl_pitch, &style_angle, 0);
    lv_label_set_text(lbl_pitch, "Pitch: 0.0");
    lv_obj_align(lbl_pitch, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_roll = lv_label_create(col_right);
    lv_obj_add_style(lbl_roll, &style_angle, 0);
    lv_label_set_text(lbl_roll, "Roll:  0.0");
    lv_obj_align(lbl_roll, LV_ALIGN_TOP_LEFT, 0, 30);

    /* Artificial horizon line */
    static lv_point_t line_pts[2] = {{0, 86}, {640, 86}};
    horizon_line = lv_line_create(scr);
    lv_line_set_points(horizon_line, line_pts, 2);
    lv_obj_set_style_line_color(horizon_line, lv_color_hex(g_theme_dark ? 0x44FF44 : 0x228822), 0);
    lv_obj_set_style_line_width(horizon_line, 2, 0);
    lv_obj_set_style_line_opa(horizon_line, LV_OPA_40, 0);

    /* ── Page indicator dots ──── */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 80, 12);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 2) {  /* MODE_COMPASS is index 2 */
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        } else {
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
        }
    }

    lv_disp_load_scr(scr);
    ESP_LOGI(TAG, "Compass screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_compass_destroy(void)
{
    if (scr) {
        lv_obj_del(scr);
        scr = NULL;
    }
}

/* ── Update ───────────────────────────────────────────────── */
void screen_compass_update(void)
{
    if (scr == NULL || !imu_initialized) return;

    imu_raw_t d = read_imu();

    /* Accel scale for ±8g = 8/32768 */
    float ax_g = d.ax / 4096.0f;
    float ay_g = d.ay / 4096.0f;
    float az_g = d.az / 4096.0f;

    lv_label_set_text_fmt(lbl_accel, "Accel: %.2f  %.2f  %.2f g",
                          ax_g, ay_g, az_g);
    lv_label_set_text_fmt(lbl_gyro,  "Gyro:  %d  %d  %d",
                          d.gx, d.gy, d.gz);

    /* Simple pitch/roll from accel */
    float pitch = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * 57.2958f;
    float roll  = atan2f(ay_g, az_g) * 57.2958f;

    lv_label_set_text_fmt(lbl_pitch, "Pitch: %.1f", pitch);
    lv_label_set_text_fmt(lbl_roll,  "Roll:  %.1f", roll);
}
