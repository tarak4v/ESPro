/**
 * @file main.c
 * @brief SS_Smallstart — MeowKit-inspired multi-mode UI for
 *        Waveshare ESP32-S3-Touch-LCD-3.49
 *
 * Boots directly to Clock screen.  Swipe left/right to switch modes.
 * IMU accelerometer used for orientation flip detection.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "esp_lcd_axs15231b.h"

#include "hw_config.h"
#include "i2c_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "app_manager.h"
#include "screen_clock.h"
#include "wifi_time.h"
#include "boot_sound.h"
#include "screen_settings.h"
#include "screen_tamafi.h"
#include "weather.h"
#include "sd_log.h"
#include "macropad.h"

static const char *TAG = "main";

/* ── LVGL synchronisation ─────────────────────────────────── */
static SemaphoreHandle_t lvgl_mux     = NULL;
static SemaphoreHandle_t flush_sema   = NULL;
static uint16_t         *dma_buf      = NULL;

#if (DISP_ROTATION == DISP_ROT_90)
static uint16_t *rotat_buf = NULL;
#endif

/* ── Orientation flip (IMU-based) ─────────────────────────── */
static volatile bool s_display_flipped = false;
static int  flip_debounce = 0;
#define FLIP_DEBOUNCE_COUNT  10   /* 10 × 100 ms = 1 s sustained flip */

/* ── Display init commands ────────────────────────────────── */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

/* ── Callbacks & helpers ──────────────────────────────────── */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t woken;
    xSemaphoreGiveFromISR(flush_sema, &woken);
    return false;
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool lvgl_lock(int timeout_ms)
{
    const TickType_t ticks = (timeout_ms == -1)
        ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

void lvgl_unlock(void)
{
    xSemaphoreGive(lvgl_mux);
}

/* ── LVGL flush callback ─────────────────────────────────── */
static void lvgl_flush_cb(lv_disp_drv_t *drv,
                           const lv_area_t *area,
                           lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;

#if (DISP_ROTATION == DISP_ROT_90)
    /* Software 90° rotation */
    uint16_t *src = (uint16_t *)color_map;
    uint32_t idx = 0;
    for (uint16_t j = 0; j < LCD_H_RES; j++) {
        for (uint16_t i = 0; i < LCD_V_RES; i++) {
            rotat_buf[idx++] = src[LCD_H_RES * (LCD_V_RES - i - 1) + j];
        }
    }

    /* If device is upside-down, reverse the entire buffer (180° flip) */
    if (s_display_flipped) {
        int32_t n = LCD_NOROT_HRES * LCD_NOROT_VRES;
        for (int32_t i = 0; i < n / 2; i++) {
            uint16_t tmp = rotat_buf[i];
            rotat_buf[i] = rotat_buf[n - 1 - i];
            rotat_buf[n - 1 - i] = tmp;
        }
    }

    uint16_t *map = rotat_buf;
#else
    uint16_t *map = (uint16_t *)color_map;
#endif

    const int flush_count = LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN;
    const int off_gap     = LCD_NOROT_VRES / flush_count;
    const int dma_pixels  = LVGL_DMA_BUFF_LEN / 2;

    int y1 = 0, y2 = off_gap;

    xSemaphoreGive(flush_sema);

    for (int i = 0; i < flush_count; i++) {
        xSemaphoreTake(flush_sema, portMAX_DELAY);
        memcpy(dma_buf, map, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel, 0, y1, LCD_NOROT_HRES, y2, dma_buf);
        y1 += off_gap;
        y2 += off_gap;
        map += dma_pixels;
    }
    xSemaphoreTake(flush_sema, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

/* ── Touch swipe detection ─────────────────────────────────── */
#define SWIPE_MIN_PX  40
static bool    touch_tracking = false;
static int16_t touch_start_x  = 0;
static int16_t touch_last_x   = 0;
static volatile int8_t pending_swipe = 0;

int8_t get_pending_swipe(void)
{
    int8_t s = pending_swipe;
    pending_swipe = 0;
    return s;
}

/* ── Touch input callback ─────────────────────────────────── */
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x0e, 0, 0, 0};
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));

    i2c_master_touch_write_read(disp_touch_dev_handle, cmd, 11, buf, 32);

    uint16_t px = (((uint16_t)buf[2] & 0x0f) << 8) | (uint16_t)buf[3];
    uint16_t py = (((uint16_t)buf[4] & 0x0f) << 8) | (uint16_t)buf[5];

    if (buf[1] > 0 && buf[1] < 5) {
        int16_t sx, sy;
        data->state = LV_INDEV_STATE_PR;
#if (DISP_ROTATION == DISP_ROT_NONE)
        if (px > LCD_V_RES) px = LCD_V_RES;
        if (py > LCD_H_RES) py = LCD_H_RES;
        sx = py;
        sy = LCD_V_RES - px;
#else
        if (px > LCD_H_RES) px = LCD_H_RES;
        if (py > LCD_V_RES) py = LCD_V_RES;
        sx = LCD_H_RES - px;
        sy = LCD_V_RES - py;
#endif
        /* Flip touch coordinates when display is flipped */
        if (s_display_flipped) {
            sx = LCD_H_RES - 1 - sx;
            sy = LCD_V_RES - 1 - sy;
        }

        data->point.x = sx;
        data->point.y = sy;

        if (!touch_tracking) {
            touch_tracking = true;
            touch_start_x  = sx;
        }
        touch_last_x = sx;
    } else {
        data->state = LV_INDEV_STATE_REL;

        if (touch_tracking) {
            touch_tracking = false;
            int16_t dx = touch_last_x - touch_start_x;
            if (dx < -SWIPE_MIN_PX) {
                pending_swipe = -1;
            } else if (dx > SWIPE_MIN_PX) {
                pending_swipe = 1;
            }
        }
    }
}

/* ── QMI8658 IMU initialisation (accel only for orientation) ── */
static void qmi8658_init(void)
{
    uint8_t who_am_i = 0;
    i2c_read_buff(imu_dev_handle, QMI8658_REG_WHO_AM_I, &who_am_i, 1);
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I: 0x%02X", who_am_i);

    uint8_t val = 0x40;  /* address auto-increment */
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL1, &val, 1);
    val = 0x25;           /* Accel ODR 250Hz, ±8g */
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL2, &val, 1);
    val = 0x01;           /* Enable accel only (no gyro needed) */
    i2c_writr_buff(imu_dev_handle, QMI8658_REG_CTRL7, &val, 1);

    ESP_LOGI(TAG, "QMI8658 IMU initialised (accel for orientation)");
}

/* ── Orientation check (called from app_update_task) ────────── */
static void check_orientation(void)
{
    uint8_t buf[6];
    if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) != 0) return;

    int16_t az = (int16_t)((buf[5] << 8) | buf[4]);
    float az_g = az / 4096.0f;   /* ±8g scale */

    bool should_flip = (az_g < -0.5f);
    if (should_flip != s_display_flipped) {
        flip_debounce++;
        if (flip_debounce >= FLIP_DEBOUNCE_COUNT) {
            s_display_flipped = should_flip;
            flip_debounce = 0;
            ESP_LOGI(TAG, "Display orientation: %s",
                     should_flip ? "FLIPPED" : "NORMAL");
        }
    } else {
        flip_debounce = 0;
    }
}

/* ── LVGL task (core 0) ──────────────────────────────────── */
static void lvgl_task(void *arg)
{
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY;
    for (;;) {
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY)  delay_ms = LVGL_TASK_MAX_DELAY;
        if (delay_ms < LVGL_TASK_MIN_DELAY)  delay_ms = LVGL_TASK_MIN_DELAY;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ── App update task (core 1) ─────────────────────────────── */
static void app_update_task(void *arg)
{
    for (;;) {
        check_orientation();
        if (lvgl_lock(100)) {
            app_manager_update();
            lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── Entry point ──────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "SS_Smallstart " FW_VERSION);

    setenv("TZ", "IST-5:30", 1);
    tzset();

    /* ── Backlight + display ──────────────────────────────── */
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    flush_sema = xSemaphoreCreateBinary();

#if (DISP_ROTATION == DISP_ROT_90)
    rotat_buf = (uint16_t *)heap_caps_malloc(
        LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(rotat_buf);
#endif

    /* ── LCD reset ────────────────────────────────────────── */
    gpio_config_t rst_cfg = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_LCD_RST),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));

    /* ── QSPI bus ─────────────────────────────────────────── */
    spi_bus_config_t bus_cfg = {
        .data0_io_num    = PIN_NUM_LCD_DATA0,
        .data1_io_num    = PIN_NUM_LCD_DATA1,
        .sclk_io_num     = PIN_NUM_LCD_PCLK,
        .data2_io_num    = PIN_NUM_LCD_DATA2,
        .data3_io_num    = PIN_NUM_LCD_DATA3,
        .max_transfer_sz = LVGL_DMA_BUFF_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* ── Panel IO (SPI) ───────────────────────────────────── */
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t    panel    = NULL;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = PIN_NUM_LCD_CS,
        .dc_gpio_num         = -1,
        .spi_mode            = 3,
        .pclk_hz             = 40 * 1000 * 1000,
        .trans_queue_depth   = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits        = 32,
        .lcd_param_bits      = 8,
        .flags = { .quad_mode = true },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &panel_io));

    /* ── Panel driver (AXS15231B) ─────────────────────────── */
    axs15231b_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
        .init_cmds      = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = -1,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = LCD_BIT_PER_PIXEL,
        .vendor_config   = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_cfg, &panel));

    gpio_set_level(PIN_NUM_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_NUM_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(PIN_NUM_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* ── LVGL init ────────────────────────────────────────── */
    lv_init();

    dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
    assert(dma_buf);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
        LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
        LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LCD_V_RES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = LCD_H_RES;
    disp_drv.ver_res      = LCD_V_RES;
    disp_drv.flush_cb     = lvgl_flush_cb;
    disp_drv.draw_buf     = &draw_buf;
    disp_drv.full_refresh = 1;
    disp_drv.user_data    = panel;
    lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
        LVGL_TICK_PERIOD_MS * 1000));

    /* ── I2C buses (touch + RTC + IMU + codec) ────────────── */
    i2c_master_Init();

    /* ── RTC init ─────────────────────────────────────────── */
    pcf85063_init();

    /* ── IMU init (for orientation detection) ─────────────── */
    qmi8658_init();

    /* ── WiFi + NTP ───────────────────────────────────────── */
    wifi_time_init();

    /* ── BLE HID macropad (must init before any BLE use) ── */
    macropad_init();

    /* Load persistent settings from NVS (volume, boot sound, 24h) */
    settings_load_from_nvs();

    /* Load TamaFi pet state from NVS */
    tamafi_load_from_nvs();

    /* ── Touch input device ───────────────────────────────── */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_t *indev = lv_indev_drv_register(&indev_drv);

    /* ── Start application (clock screen) ─────────────────── */
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    app_manager_init(indev);

    ESP_LOGI(TAG, "Starting LVGL task on core 0");
    xTaskCreatePinnedToCore(lvgl_task, "LVGL", 8192, NULL, 4, NULL, 0);

    /* Boot sound (respects settings toggle) */
    if (g_boot_sound_en)
        boot_sound_play();

    /* Weather background task */
    weather_init();

    /* SPIFFS log storage */
    sd_log_init();

    ESP_LOGI(TAG, "Starting app update task on core 1");
    xTaskCreatePinnedToCore(app_update_task, "AppMgr", 4096, NULL, 3, NULL, 1);
}
