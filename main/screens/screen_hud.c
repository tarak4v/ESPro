/**
 * @file screen_hud.c
 * @brief Productivity HUD — 5-zone display (640×172 landscape).
 *
 * Zones (left → right):
 *   1. Clock/Date       (~120 px)
 *   2. Current Task     (~180 px) + timer
 *   3. Mic/Voice Status (~80 px)
 *   4. Next Event       (~140 px)
 *   5. Wellness          (~120 px)
 *
 * Also manages the sensor_task (IMU gestures + wellness + RTC)
 * and voice integration with DeepSeek.
 */

#include "screen_hud.h"
#include "app_manager.h"
#include "hw_config.h"
#include "screen_settings.h"
#include "task_manager.h"
#include "wellness.h"
#include "gesture.h"
#include "deepseek.h"
#include "ble_calendar.h"
#include "mic_input.h"
#include "wifi_time.h"
#include "i2c_bsp.h"
#include "secrets.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

static const char *TAG = "hud";

/* ================================================================
 *  Layout constants (640 × 172)
 * ================================================================ */
#define ZONE1_X    0
#define ZONE1_W    120
#define ZONE2_X    121
#define ZONE2_W    179
#define ZONE3_X    301
#define ZONE3_W    79
#define ZONE4_X    381
#define ZONE4_W    139
#define ZONE5_X    521
#define ZONE5_W    119
#define ZONE_H     172
#define DIV_W      1
#define PAD        8

/* ================================================================
 *  LVGL handles
 * ================================================================ */
static lv_obj_t *scr = NULL;

/* Zone 1 — Clock/Date */
static lv_obj_t *z1_panel     = NULL;
static lv_obj_t *z1_time      = NULL;
static lv_obj_t *z1_date      = NULL;
static lv_obj_t *z1_weekday   = NULL;

/* Zone 2 — Task + Timer */
static lv_obj_t *z2_panel     = NULL;
static lv_obj_t *z2_task_name = NULL;
static lv_obj_t *z2_timer     = NULL;
static lv_obj_t *z2_progress  = NULL;
static lv_obj_t *z2_status    = NULL;

/* Zone 3 — Mic/Voice */
static lv_obj_t *z3_panel     = NULL;
static lv_obj_t *z3_mic_icon  = NULL;
static lv_obj_t *z3_mic_label = NULL;
static lv_obj_t *z3_resp_line = NULL;

/* Zone 4 — Next Event */
static lv_obj_t *z4_panel     = NULL;
static lv_obj_t *z4_evt_title = NULL;
static lv_obj_t *z4_evt_time  = NULL;
static lv_obj_t *z4_evt_icon  = NULL;

/* Zone 5 — Wellness */
static lv_obj_t *z5_panel     = NULL;
static lv_obj_t *z5_steps     = NULL;
static lv_obj_t *z5_score_arc = NULL;
static lv_obj_t *z5_score_lbl = NULL;

/* Mode dots */
static lv_obj_t *mode_dot[4]  = {NULL};
static lv_style_t style_dot_active, style_dot_inactive;

/* ================================================================
 *  Styles
 * ================================================================ */
static lv_style_t style_bg, style_panel, style_divider;

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, lv_color_hex(th_card));
    lv_style_set_bg_opa(&style_panel, g_theme_dark ? LV_OPA_60 : LV_OPA_COVER);
    lv_style_set_radius(&style_panel, 0);
    lv_style_set_pad_all(&style_panel, PAD);
    lv_style_set_border_width(&style_panel, 0);

    lv_style_init(&style_divider);
    lv_style_set_bg_color(&style_divider, lv_color_hex(th_label));
    lv_style_set_bg_opa(&style_divider, LV_OPA_40);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(0x00E5FF));
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

/* ── Helper: vertical divider ─────────────────────────────── */
static void create_divider(lv_obj_t *parent, int x)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_add_style(d, &style_divider, 0);
    lv_obj_set_size(d, DIV_W, ZONE_H - 20);
    lv_obj_set_pos(d, x, 10);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* ── Helper: create a zone panel ──────────────────────────── */
static lv_obj_t *create_zone(lv_obj_t *parent, int x, int w)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_add_style(p, &style_panel, 0);
    lv_obj_set_size(p, w, ZONE_H);
    lv_obj_set_pos(p, x, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

/* ================================================================
 *  BCD / RTC helpers
 * ================================================================ */
static inline uint8_t bcd2dec(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static const char *weekday_names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

/* ================================================================
 *  Sensor task — IMU gestures + wellness + timer tick
 * ================================================================ */
static TaskHandle_t s_sensor_task_handle = NULL;
static volatile bool s_sensor_running = false;

static void sensor_task(void *arg)
{
    int tick_acc = 0;   /* accumulate to 1-second intervals for timer */
    int save_acc = 0;   /* accumulate for periodic save */

    while (s_sensor_running) {
        /* Read IMU accel */
        uint8_t buf[6];
        if (i2c_read_buff(imu_dev_handle, QMI8658_REG_AX_L, buf, 6) == 0) {
            int16_t ax_raw = (int16_t)((buf[1] << 8) | buf[0]);
            int16_t ay_raw = (int16_t)((buf[3] << 8) | buf[2]);
            int16_t az_raw = (int16_t)((buf[5] << 8) | buf[4]);
            float ax = ax_raw / 4096.0f;
            float ay = ay_raw / 4096.0f;
            float az = az_raw / 4096.0f;
            float mag = sqrtf(ax * ax + ay * ay + az * az);

            /* Feed wellness (step counter) */
            wellness_feed_accel(mag);

            /* Feed gesture detector */
            gesture_t g = gesture_feed(ax, ay, az);
            if (g == GESTURE_DOUBLE_TAP) {
                task_manager_timer_snooze();
            } else if (g == GESTURE_FORWARD_TILT) {
                /* Acknowledge — dismiss current alert/notification */
                ESP_LOGI(TAG, "Alert acknowledged (tilt)");
            }
        }

        /* Timer tick (~1 second = 50 iterations × 20 ms) */
        tick_acc++;
        if (tick_acc >= 50) {
            tick_acc = 0;
            task_manager_timer_tick();
        }

        /* Periodic save (every 60 s = 3000 × 20 ms) */
        save_acc++;
        if (save_acc >= 3000) {
            save_acc = 0;
            wellness_save();
        }

        vTaskDelay(pdMS_TO_TICKS(20));  /* ~50 Hz */
    }

    s_sensor_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  Voice task — continuous mic monitoring + Groq STT + DeepSeek
 * ================================================================ */
static TaskHandle_t s_voice_task_handle = NULL;
static volatile bool s_voice_running = false;

typedef enum {
    VOICE_IDLE,
    VOICE_LISTENING,
    VOICE_PROCESSING,
} voice_state_t;

static volatile voice_state_t s_voice_state = VOICE_IDLE;

/* STT constants */
#define HUD_REC_RATE     16000
#define HUD_MAX_REC_SEC  5
#define HUD_MAX_REC_BYTES (HUD_REC_RATE * 2 * HUD_MAX_REC_SEC)  /* 160 KB */
#define HUD_JSON_BUF     (4 * 1024)

#define GROQ_STT_URL     "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_STT_MODEL   "whisper-large-v3-turbo"

/* Wake detection thresholds */
#define WAKE_LEVEL_THRESH  35   /* mic_get_level() 0-100 scale */
#define WAKE_SUSTAIN_CNT   6    /* 6 × 50ms = 300ms sustained voice */
#define SILENCE_THRESH     30   /* level below this = silence */
#define SILENCE_TIMEOUT_CNT 30  /* 30 × 50ms = 1.5s silence = stop */

static int16_t *s_hud_rec_buf = NULL;

/* WAV header builder */
static void hud_build_wav_header(uint8_t *hdr, size_t pcm_bytes)
{
    uint32_t file_size = 36 + pcm_bytes;
    uint32_t byte_rate = HUD_REC_RATE * 1 * 2;
    uint16_t block_align = 2;

    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4,  &file_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t chunk_size = 16;
    memcpy(hdr + 16, &chunk_size, 4);
    uint16_t fmt = 1;
    memcpy(hdr + 20, &fmt, 2);
    uint16_t channels = 1;
    memcpy(hdr + 22, &channels, 2);
    uint32_t sr = HUD_REC_RATE;
    memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    uint16_t bps = 16;
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* HTTP response accumulator */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} hud_resp_t;

static esp_err_t hud_http_evt(esp_http_client_event_t *evt)
{
    hud_resp_t *rb = (hud_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (rb->len + evt->data_len < rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* Send audio to Groq Whisper STT → return transcript (caller frees) */
static char *hud_stt_transcribe(size_t rec_bytes)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "STT: no WiFi");
        return NULL;
    }

    static const char *boundary = "----HUDBoundary";

    uint8_t wav_hdr[44];
    hud_build_wav_header(wav_hdr, rec_bytes);

    char preamble[512];
    int pre_len = snprintf(preamble, sizeof(preamble),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "en\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, GROQ_STT_MODEL, boundary, boundary);

    char epilogue[64];
    int epi_len = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", boundary);

    size_t total_len = pre_len + 44 + rec_bytes + epi_len;

    char *json_buf = heap_caps_calloc(1, HUD_JSON_BUF, MALLOC_CAP_SPIRAM);
    if (!json_buf) return NULL;
    hud_resp_t rb = { .buf = json_buf, .len = 0, .cap = HUD_JSON_BUF - 1 };

    char auth_hdr[128];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", GROQ_API_KEY);
    char ct[80];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t cfg = {
        .url               = GROQ_STT_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 30000,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = hud_http_evt,
        .user_data         = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", ct);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(json_buf);
        return NULL;
    }

    /* Send multipart body */
    esp_http_client_write(client, preamble, pre_len);
    esp_http_client_write(client, (char *)wav_hdr, 44);

    /* Send PCM in 2 KB chunks */
    size_t sent = 0;
    while (sent < rec_bytes) {
        size_t chunk = rec_bytes - sent;
        if (chunk > 2048) chunk = 2048;
        esp_http_client_write(client, (char *)s_hud_rec_buf + sent, chunk);
        sent += chunk;
    }
    esp_http_client_write(client, epilogue, epi_len);

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;
    esp_http_client_read_response(client, rb.buf + rb.len, rb.cap - rb.len);
    rb.buf[rb.cap - 1] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "STT HTTP %d", status);
        heap_caps_free(json_buf);
        return NULL;
    }

    /* Parse transcript from JSON */
    cJSON *root = cJSON_Parse(json_buf);
    heap_caps_free(json_buf);
    if (!root) return NULL;

    char *transcript = NULL;
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
        transcript = strdup(text->valuestring);
    }
    cJSON_Delete(root);

    if (transcript) {
        ESP_LOGI(TAG, "STT transcript: %s", transcript);
    }
    return transcript;
}

static void voice_task(void *arg)
{
    int loud_count = 0;
    int silence_count = 0;

    /* Start mic immediately for continuous level monitoring */
    mic_start();
    ESP_LOGI(TAG, "Voice task: mic started for HUD monitoring");

    /* Alloc recording buffer in PSRAM */
    s_hud_rec_buf = heap_caps_malloc(HUD_MAX_REC_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_hud_rec_buf) {
        ESP_LOGE(TAG, "Failed to alloc HUD rec buffer");
        s_voice_running = false;
        vTaskDelete(NULL);
        return;
    }

    while (s_voice_running) {
        if (s_voice_state == VOICE_IDLE) {
            /* Monitor mic level for voice activity */
            uint8_t level = mic_get_level();

            if (level > WAKE_LEVEL_THRESH) {
                loud_count++;
                if (loud_count >= WAKE_SUSTAIN_CNT) {
                    /* Voice detected — start recording */
                    s_voice_state = VOICE_LISTENING;
                    mic_set_record_buffer(s_hud_rec_buf, HUD_MAX_REC_BYTES);
                    silence_count = 0;
                    loud_count = 0;
                    ESP_LOGI(TAG, "Voice wake! Recording (level=%d)...", level);
                }
            } else {
                loud_count = 0;
            }

        } else if (s_voice_state == VOICE_LISTENING) {
            /* Record until silence or max duration */
            uint8_t level = mic_get_level();

            if (level < SILENCE_THRESH) {
                silence_count++;
            } else {
                silence_count = 0;
            }

            size_t rec_bytes = mic_get_recorded_bytes();
            bool timeout = (rec_bytes >= HUD_MAX_REC_BYTES);
            bool silent  = (silence_count >= SILENCE_TIMEOUT_CNT && rec_bytes > 3200);

            if (timeout || silent) {
                /* Get recorded bytes BEFORE clearing buffer */
                rec_bytes = mic_get_recorded_bytes();
                mic_clear_record_buffer();

                ESP_LOGI(TAG, "Recording done: %u bytes (%.1fs), reason=%s",
                         (unsigned)rec_bytes, (float)rec_bytes / (HUD_REC_RATE * 2),
                         timeout ? "max" : "silence");

                s_voice_state = VOICE_PROCESSING;

                if (rec_bytes < 1600) {
                    ESP_LOGW(TAG, "Too short — ignoring");
                    s_voice_state = VOICE_IDLE;
                    continue;
                }

                /* Try STT → DeepSeek pipeline */
                char *transcript = hud_stt_transcribe(rec_bytes);
                if (transcript) {
                    /* Run through DeepSeek (local intent first, then API) */
                    deepseek_query(transcript);
                    free(transcript);
                } else {
                    /* STT failed — try offline */
                    snprintf((char *)g_ds_result.text, DEEPSEEK_RESP_MAX,
                             "Couldn't hear that. Try again.");
                    g_ds_result.ok = true;
                    ESP_LOGW(TAG, "STT failed — no transcript");
                }

                s_voice_state = VOICE_IDLE;
                silence_count = 0;
            }

        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Cleanup */
    mic_stop();
    if (s_hud_rec_buf) {
        heap_caps_free(s_hud_rec_buf);
        s_hud_rec_buf = NULL;
    }

    s_voice_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  Screen create
 * ================================================================ */
void screen_hud_create(void)
{
    init_styles();

    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Zone 1: Clock / Date ─────────────────────────────── */
    z1_panel = create_zone(scr, ZONE1_X, ZONE1_W);

    z1_time = lv_label_create(z1_panel);
    lv_obj_set_style_text_font(z1_time, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(z1_time, lv_color_hex(th_text), 0);
    lv_label_set_text(z1_time, "00:00");
    lv_obj_set_pos(z1_time, 4, 20);

    z1_date = lv_label_create(z1_panel);
    lv_obj_set_style_text_font(z1_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z1_date, lv_color_hex(th_label), 0);
    lv_label_set_text(z1_date, "Mar 21");
    lv_obj_set_pos(z1_date, 4, 70);

    z1_weekday = lv_label_create(z1_panel);
    lv_obj_set_style_text_font(z1_weekday, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(z1_weekday, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(z1_weekday, "SAT");
    lv_obj_set_pos(z1_weekday, 4, 88);

    create_divider(scr, ZONE1_W);

    /* ── Zone 2: Task + Timer ─────────────────────────────── */
    z2_panel = create_zone(scr, ZONE2_X, ZONE2_W);

    lv_obj_t *z2_title = lv_label_create(z2_panel);
    lv_obj_set_style_text_font(z2_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z2_title, lv_color_hex(th_label), 0);
    lv_label_set_text(z2_title, "CURRENT TASK");
    lv_obj_set_pos(z2_title, 0, 2);

    z2_task_name = lv_label_create(z2_panel);
    lv_obj_set_style_text_font(z2_task_name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(z2_task_name, lv_color_hex(th_text), 0);
    lv_label_set_long_mode(z2_task_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(z2_task_name, ZONE2_W - PAD * 2);
    lv_label_set_text(z2_task_name, "---");
    lv_obj_set_pos(z2_task_name, 0, 22);

    z2_timer = lv_label_create(z2_panel);
    lv_obj_set_style_text_font(z2_timer, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(z2_timer, lv_color_hex(0x00E676), 0);
    lv_label_set_text(z2_timer, "--:--");
    lv_obj_set_pos(z2_timer, 0, 50);

    /* Progress bar (task completion) */
    z2_progress = lv_bar_create(z2_panel);
    lv_obj_set_size(z2_progress, ZONE2_W - PAD * 2, 8);
    lv_obj_set_pos(z2_progress, 0, 105);
    lv_bar_set_range(z2_progress, 0, 100);
    lv_bar_set_value(z2_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(z2_progress, lv_color_hex(th_btn), 0);
    lv_obj_set_style_bg_color(z2_progress, lv_color_hex(0x00E676),
                              LV_PART_INDICATOR);

    z2_status = lv_label_create(z2_panel);
    lv_obj_set_style_text_font(z2_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z2_status, lv_color_hex(th_label), 0);
    lv_label_set_text(z2_status, "Tap to start");
    lv_obj_set_pos(z2_status, 0, 120);

    create_divider(scr, ZONE2_X + ZONE2_W);

    /* ── Zone 3: Mic / Voice ──────────────────────────────── */
    z3_panel = create_zone(scr, ZONE3_X, ZONE3_W);

    z3_mic_icon = lv_label_create(z3_panel);
    lv_obj_set_style_text_font(z3_mic_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(z3_mic_icon, lv_color_hex(th_label), 0);
    lv_label_set_text(z3_mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_align(z3_mic_icon, LV_ALIGN_TOP_MID, 0, 15);

    z3_mic_label = lv_label_create(z3_panel);
    lv_obj_set_style_text_font(z3_mic_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z3_mic_label, lv_color_hex(th_label), 0);
    lv_label_set_text(z3_mic_label, "IDLE");
    lv_obj_align(z3_mic_label, LV_ALIGN_TOP_MID, 0, 55);

    z3_resp_line = lv_label_create(z3_panel);
    lv_obj_set_style_text_font(z3_resp_line, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z3_resp_line, lv_color_hex(0x00E5FF), 0);
    lv_label_set_long_mode(z3_resp_line, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(z3_resp_line, ZONE3_W - PAD * 2);
    lv_label_set_text(z3_resp_line, "");
    lv_obj_set_pos(z3_resp_line, 0, 80);

    create_divider(scr, ZONE3_X + ZONE3_W);

    /* ── Zone 4: Next Event ───────────────────────────────── */
    z4_panel = create_zone(scr, ZONE4_X, ZONE4_W);

    lv_obj_t *z4_hdr = lv_label_create(z4_panel);
    lv_obj_set_style_text_font(z4_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z4_hdr, lv_color_hex(th_label), 0);
    lv_label_set_text(z4_hdr, "NEXT EVENT");
    lv_obj_set_pos(z4_hdr, 0, 2);

    z4_evt_icon = lv_label_create(z4_panel);
    lv_obj_set_style_text_font(z4_evt_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(z4_evt_icon, lv_color_hex(0xFFAB40), 0);
    lv_label_set_text(z4_evt_icon, LV_SYMBOL_BELL);
    lv_obj_set_pos(z4_evt_icon, 0, 25);

    z4_evt_title = lv_label_create(z4_panel);
    lv_obj_set_style_text_font(z4_evt_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(z4_evt_title, lv_color_hex(th_text), 0);
    lv_label_set_long_mode(z4_evt_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(z4_evt_title, ZONE4_W - PAD * 2);
    lv_label_set_text(z4_evt_title, "No events");
    lv_obj_set_pos(z4_evt_title, 0, 55);

    z4_evt_time = lv_label_create(z4_panel);
    lv_obj_set_style_text_font(z4_evt_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(z4_evt_time, lv_color_hex(0xFFAB40), 0);
    lv_label_set_text(z4_evt_time, "--:--");
    lv_obj_set_pos(z4_evt_time, 0, 80);

    create_divider(scr, ZONE4_X + ZONE4_W);

    /* ── Zone 5: Wellness ─────────────────────────────────── */
    z5_panel = create_zone(scr, ZONE5_X, ZONE5_W);

    lv_obj_t *z5_hdr = lv_label_create(z5_panel);
    lv_obj_set_style_text_font(z5_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z5_hdr, lv_color_hex(th_label), 0);
    lv_label_set_text(z5_hdr, "WELLNESS");
    lv_obj_set_pos(z5_hdr, 0, 2);

    /* Arc gauge for wellness score */
    z5_score_arc = lv_arc_create(z5_panel);
    lv_obj_set_size(z5_score_arc, 70, 70);
    lv_obj_set_pos(z5_score_arc, (ZONE5_W - PAD * 2 - 70) / 2, 20);
    lv_arc_set_rotation(z5_score_arc, 135);
    lv_arc_set_bg_angles(z5_score_arc, 0, 270);
    lv_arc_set_range(z5_score_arc, 0, 100);
    lv_arc_set_value(z5_score_arc, 0);
    lv_obj_remove_style(z5_score_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(z5_score_arc, lv_color_hex(th_btn), LV_PART_MAIN);
    lv_obj_set_style_arc_color(z5_score_arc, lv_color_hex(0x00E676),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(z5_score_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(z5_score_arc, 6, LV_PART_INDICATOR);
    lv_obj_clear_flag(z5_score_arc, LV_OBJ_FLAG_CLICKABLE);

    z5_score_lbl = lv_label_create(z5_panel);
    lv_obj_set_style_text_font(z5_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(z5_score_lbl, lv_color_hex(th_text), 0);
    lv_label_set_text(z5_score_lbl, "0%");
    lv_obj_align_to(z5_score_lbl, z5_score_arc, LV_ALIGN_CENTER, 0, 0);

    z5_steps = lv_label_create(z5_panel);
    lv_obj_set_style_text_font(z5_steps, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(z5_steps, lv_color_hex(th_label), 0);
    lv_label_set_text(z5_steps, "0 steps");
    lv_obj_set_pos(z5_steps, 0, 100);

    /* ── Mode dots (bottom centre) ────────────────────────── */
    int dot_y = LCD_V_RES - 14;
    int dot_total_w = 4 * 10;
    int dot_x0 = (LCD_H_RES - dot_total_w) / 2;
    for (int i = 0; i < 4; i++) {
        mode_dot[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(mode_dot[i]);
        /* HUD is index 3 in swipe cycle (Clock=0, Menu=1, More=2, HUD=3) */
        lv_obj_add_style(mode_dot[i],
                         (i == 3) ? &style_dot_active : &style_dot_inactive, 0);
        lv_obj_set_pos(mode_dot[i], dot_x0 + i * 10, dot_y);
        lv_obj_clear_flag(mode_dot[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    /* ── Set as pending screen ────────────────────────────── */
    g_pending_scr = scr;

    /* ── Start background tasks ───────────────────────────── */
    s_sensor_running = true;
    xTaskCreatePinnedToCore(sensor_task, "SensorHUD", 2048, NULL, 3, &s_sensor_task_handle, 0);

    s_voice_running = true;
    xTaskCreatePinnedToCoreWithCaps(voice_task, "VoiceHUD", 24576, NULL, 2,
                                    &s_voice_task_handle, 0, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "HUD screen created");
}

/* ================================================================
 *  Screen update (~100 ms from app_manager)
 * ================================================================ */
void screen_hud_update(void)
{
    if (!scr) return;

    /* ── Zone 1: Clock ────────────────────────────────────── */
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (g_clock_24h) {
        lv_label_set_text_fmt(z1_time, "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        int h12 = ti.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        lv_label_set_text_fmt(z1_time, "%2d:%02d", h12, ti.tm_min);
    }

    static const char *mon_names[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    lv_label_set_text_fmt(z1_date, "%s %d", mon_names[ti.tm_mon], ti.tm_mday);

    if (ti.tm_wday >= 0 && ti.tm_wday < 7)
        lv_label_set_text(z1_weekday, weekday_names[ti.tm_wday]);

    /* ── Zone 2: Task + Timer ─────────────────────────────── */
    if (g_tasks.count > 0) {
        lv_label_set_text(z2_task_name, g_tasks.items[g_tasks.current].title);

        char tbuf[8];
        task_manager_timer_str(tbuf, sizeof(tbuf));
        lv_label_set_text(z2_timer, tbuf);

        /* Timer color: green running, yellow paused, red expired */
        if (g_tasks.timer_remain_sec <= 0) {
            lv_obj_set_style_text_color(z2_timer, lv_color_hex(0xFF5252), 0);
            lv_label_set_text(z2_status, "Timer done!");
        } else if (g_tasks.timer_running) {
            lv_obj_set_style_text_color(z2_timer, lv_color_hex(0x00E676), 0);
            lv_label_set_text(z2_status, "Running...");
        } else {
            lv_obj_set_style_text_color(z2_timer, lv_color_hex(0xFFD740), 0);
            lv_label_set_text(z2_status, "Paused");
        }

        /* Progress: % of tasks done */
        int done_count = 0;
        for (int i = 0; i < g_tasks.count; i++)
            if (g_tasks.items[i].done) done_count++;
        lv_bar_set_value(z2_progress,
                         (done_count * 100) / g_tasks.count, LV_ANIM_ON);
    }

    /* ── Zone 3: Mic status ───────────────────────────────── */
    switch (s_voice_state) {
        case VOICE_IDLE:
            lv_label_set_text(z3_mic_label, "IDLE");
            lv_obj_set_style_text_color(z3_mic_icon, lv_color_hex(th_label), 0);
            break;
        case VOICE_LISTENING:
            lv_label_set_text(z3_mic_label, "LISTEN");
            lv_obj_set_style_text_color(z3_mic_icon, lv_color_hex(0xFF5252), 0);
            break;
        case VOICE_PROCESSING:
            lv_label_set_text(z3_mic_label, "THINK");
            lv_obj_set_style_text_color(z3_mic_icon, lv_color_hex(0xAA00FF), 0);
            break;
    }

    /* Show last DeepSeek response */
    if (g_ds_result.ok && g_ds_result.text[0] != '\0') {
        lv_label_set_text(z3_resp_line, (const char *)g_ds_result.text);
    }

    /* ── Zone 4: Next event ───────────────────────────────── */
    const cal_event_t *next = ble_calendar_next_event();
    if (next) {
        lv_label_set_text(z4_evt_title, next->title);
        lv_label_set_text_fmt(z4_evt_time, "%02d:%02d", next->hour, next->minute);
    } else {
        lv_label_set_text(z4_evt_title, "No events");
        lv_label_set_text(z4_evt_time, "--:--");
    }

    /* ── Zone 5: Wellness ─────────────────────────────────── */
    uint8_t score = wellness_score();
    lv_arc_set_value(z5_score_arc, score);
    lv_label_set_text_fmt(z5_score_lbl, "%d%%", score);
    lv_obj_align_to(z5_score_lbl, z5_score_arc, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text_fmt(z5_steps, "%lu steps", (unsigned long)g_wellness.steps);
}

/* ================================================================
 *  Screen destroy
 * ================================================================ */
void screen_hud_destroy(void)
{
    /* Stop background tasks */
    s_sensor_running = false;
    s_voice_running  = false;

    /* Wait briefly for tasks to exit */
    if (s_sensor_task_handle) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_voice_task_handle)  vTaskDelay(pdMS_TO_TICKS(100));

    /* Save state */
    wellness_save();
    task_manager_save();

    /* Null all widget pointers before deleting */
    z1_panel = z1_time = z1_date = z1_weekday = NULL;
    z2_panel = z2_task_name = z2_timer = z2_progress = z2_status = NULL;
    z3_panel = z3_mic_icon = z3_mic_label = z3_resp_line = NULL;
    z4_panel = z4_evt_title = z4_evt_time = z4_evt_icon = NULL;
    z5_panel = z5_score_arc = z5_score_lbl = z5_steps = NULL;
    for (int i = 0; i < 4; i++) mode_dot[i] = NULL;

    if (scr) {
        lv_obj_del(scr);
        scr = NULL;
    }

    ESP_LOGI(TAG, "HUD screen destroyed");
}
