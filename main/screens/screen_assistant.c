/**
 * @file screen_assistant.c
 * @brief Siri-style AI voice assistant overlay.
 *
 * UI layout (640×172):
 *   Left  – round animated mic button (~120 px)
 *   Right – transcript, thin horizontal waveform, AI response text
 *
 * Pipeline (direct — no relay server):
 *   Mic → HTTPS POST WAV to Groq Whisper STT
 *       → HTTPS POST JSON to Groq LLM
 *       → display transcript + response text
 */

#include "screen_assistant.h"
#include "hw_config.h"
#include "screen_settings.h"
#include "mic_input.h"
#include "music_player.h"
#include "wifi_time.h"
#include "i2c_bsp.h"

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
#include <math.h>

/* TTS audio playback */
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "assistant";

/* ================================================================
 *  Layout constants (640 × 172)
 * ================================================================ */
#define SIRI_SIZE        86         /* button diameter */
#define SIRI_CX          58         /* button centre X */
#define SIRI_CY          98         /* button centre Y */
#define RING_SIZE        100        /* outer glow ring diameter */

#define PANEL_X          130        /* right panel left edge */
#define PANEL_W          500        /* right panel width */

#define WAVE_BAR_CNT     50
#define WAVE_BAR_W       8
#define WAVE_BAR_GAP     2
#define WAVE_MAX_H       10
#define WAVE_Y           160        /* waveform top Y — bottom strip */

/* ================================================================
 *  Audio / network constants
 * ================================================================ */
#define REC_SAMPLE_RATE  16000
#define MAX_REC_SEC      10
#define MAX_REC_BYTES    (REC_SAMPLE_RATE * 2 * MAX_REC_SEC)   /* 320 KB */
#define MAX_JSON_RESP    (8 * 1024)                             /* 8 KB for JSON */

/* Groq API endpoints */
#define GROQ_STT_URL    "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_CHAT_URL   "https://api.groq.com/openai/v1/chat/completions"
#define GROQ_STT_MODEL  "whisper-large-v3-turbo"
#define GROQ_LLM_MODEL  "llama-3.3-70b-versatile"

#define SYSTEM_PROMPT    "You are ESPro, a helpful, concise AI assistant on a smartwatch. " \
                         "Keep responses under 40 words — they will be shown " \
                         "on a tiny 640x172 pixel screen. Be friendly, direct, and useful."

/* Groq TTS */
#define GROQ_TTS_URL    "https://api.groq.com/openai/v1/audio/speech"
#define GROQ_TTS_MODEL  "playai-tts"
#define GROQ_TTS_VOICE  "Fritz-PlayAI"
#define TTS_MAX_AUDIO   (512 * 1024)   /* 512 KB max TTS WAV response */

/* ================================================================
 *  State machine
 * ================================================================ */
typedef enum {
    AST_IDLE,
    AST_LISTENING,
    AST_PROCESSING,
    AST_SPEAKING,
} assist_state_t;

static volatile assist_state_t s_state   = AST_IDLE;
static volatile bool s_active            = false;
static volatile bool s_abort             = false;
static volatile bool s_http_done         = false;
static volatile bool s_http_ok           = false;

/* ================================================================
 *  Audio buffers (PSRAM)
 * ================================================================ */
static int16_t *s_rec_buf   = NULL;     /* recording buffer */

/* Parsed response */
static char     s_transcript[512];
static char     s_response[1024];
static char     s_error[128];

/* ================================================================
 *  LVGL objects
 * ================================================================ */
static lv_obj_t *s_overlay        = NULL;
static lv_obj_t *s_siri_btn      = NULL;
static lv_obj_t *s_siri_ring     = NULL;
static lv_obj_t *s_siri_icon     = NULL;
static lv_obj_t *s_status_lbl    = NULL;
static lv_obj_t *s_transcript_lbl = NULL;
static lv_obj_t *s_response_lbl  = NULL;
static lv_obj_t *s_wave_bars[WAVE_BAR_CNT];

/* ── Waveform levels ── */
static uint8_t  s_wave_levels[WAVE_BAR_CNT];
static lv_timer_t *s_wave_timer = NULL;

/* ── Pulse phase (0..359) ── */
static int s_pulse = 0;

/* ── Task handles ── */
static TaskHandle_t s_http_task_h = NULL;

/* ── Failsafe: watchdog for stuck PROCESSING state ── */
static uint32_t s_processing_start = 0;
#define PROCESSING_TIMEOUT_MS  45000   /* 45 s max for STT+LLM */

/* ================================================================
 *  Colour palette per state
 * ================================================================ */
#define COL_IDLE      0x333344
#define COL_LISTEN    0xCC2233
#define COL_PROCESS   0x7B1FA2
#define COL_SPEAK     0x1565C0
#define COL_RING_IDLE 0x444455
#define COL_WAVE_IDLE 0x444455
#define COL_WAVE_LIS  0xFF4455
#define COL_WAVE_SPK  0x42A5F5

/* ================================================================
 *  Forward declarations
 * ================================================================ */
static void siri_btn_cb(lv_event_t *e);
static void back_btn_cb(lv_event_t *e);
static void wave_timer_cb(lv_timer_t *t);
static void start_listening(void);
static void stop_listening(void);
static void http_task(void *arg);
static void set_state(assist_state_t st);
static void free_buffers(void);

/* ================================================================
 *  UI helpers
 * ================================================================ */
static uint32_t state_color(void)
{
    switch (s_state) {
        case AST_LISTENING:  return COL_LISTEN;
        case AST_PROCESSING: return COL_PROCESS;
        case AST_SPEAKING:   return COL_SPEAK;
        default:             return COL_IDLE;
    }
}

static uint32_t wave_color(void)
{
    switch (s_state) {
        case AST_LISTENING:  return COL_WAVE_LIS;
        case AST_SPEAKING:   return COL_WAVE_SPK;
        default:             return COL_WAVE_IDLE;
    }
}

static const char *state_icon(void)
{
    switch (s_state) {
        case AST_LISTENING:  return LV_SYMBOL_STOP;
        case AST_PROCESSING: return LV_SYMBOL_REFRESH;
        case AST_SPEAKING:   return LV_SYMBOL_VOLUME_MAX;
        default:             return LV_SYMBOL_AUDIO;
    }
}

static const char *state_text(void)
{
    switch (s_state) {
        case AST_LISTENING:  return "Listening...";
        case AST_PROCESSING: return "Thinking...";
        case AST_SPEAKING:   return "Speaking...";
        default:             return "Tap to speak";
    }
}

/* ── Update button appearance ── */
static void update_button_style(void)
{
    if (!s_siri_btn) return;
    uint32_t col = state_color();

    lv_obj_set_style_bg_color(s_siri_btn, lv_color_hex(col), 0);
    lv_obj_set_style_shadow_color(s_siri_btn, lv_color_hex(col), 0);

    if (s_siri_ring)
        lv_obj_set_style_bg_color(s_siri_ring, lv_color_hex(col), 0);

    if (s_siri_icon)
        lv_label_set_text(s_siri_icon, state_icon());

    if (s_status_lbl)
        lv_label_set_text(s_status_lbl, state_text());
}

/* ================================================================
 *  Waveform animation timer (runs every 60 ms)
 * ================================================================ */
static void wave_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    /* Shift left */
    memmove(s_wave_levels, s_wave_levels + 1, WAVE_BAR_CNT - 1);

    /* New sample on right */
    uint8_t level = 0;
    if (s_state == AST_LISTENING) {
        level = mic_get_level();
    }
    s_wave_levels[WAVE_BAR_CNT - 1] = level;

    /* Update bar heights + colours */
    uint32_t wcol = wave_color();
    for (int i = 0; i < WAVE_BAR_CNT; i++) {
        if (!s_wave_bars[i]) continue;
        int h = (s_wave_levels[i] * WAVE_MAX_H) / 100;
        if (h < 2) h = 2;
        lv_obj_set_height(s_wave_bars[i], h);
        lv_obj_set_style_bg_color(s_wave_bars[i], lv_color_hex(wcol), 0);
    }

    /* Pulse animation for button */
    s_pulse = (s_pulse + 12) % 360;
    if (s_siri_ring && (s_state == AST_LISTENING || s_state == AST_SPEAKING)) {
        float sine = sinf(s_pulse * 3.14159f / 180.0f);
        lv_opa_t opa = (lv_opa_t)(120 + (int)(80.0f * sine));
        lv_obj_set_style_bg_opa(s_siri_ring, opa, 0);
    } else if (s_siri_ring) {
        lv_obj_set_style_bg_opa(s_siri_ring, 60, 0);
    }
}

/* ================================================================
 *  Recording
 * ================================================================ */
static void start_listening(void)
{
    /* Allocate recording buffer in PSRAM */
    s_rec_buf = heap_caps_malloc(MAX_REC_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_rec_buf) {
        ESP_LOGE(TAG, "Failed to alloc rec buffer (%u bytes)", (unsigned)MAX_REC_BYTES);
        return;
    }
    ESP_LOGI(TAG, "Rec buffer allocated: %u bytes at %p", (unsigned)MAX_REC_BYTES, s_rec_buf);

    /* Set recording buffer and start mic */
    mic_set_record_buffer(s_rec_buf, MAX_REC_BYTES);
    mic_start();

    set_state(AST_LISTENING);
    ESP_LOGI(TAG, "Recording started — tap again to stop");
}

static void stop_listening(void)
{
    ESP_LOGI(TAG, "Stopping recording...");
    mic_stop();
    size_t rec_bytes = mic_get_recorded_bytes();
    mic_clear_record_buffer();

    ESP_LOGI(TAG, "Recorded %u bytes (~%.1f s)",
             (unsigned)rec_bytes, (float)rec_bytes / (REC_SAMPLE_RATE * 2));

    if (rec_bytes < 1600) {
        ESP_LOGW(TAG, "Too short — ignoring");
        free(s_rec_buf); s_rec_buf = NULL;
        set_state(AST_IDLE);
        return;
    }

    set_state(AST_PROCESSING);

    /* Launch HTTP task */
    s_http_done = false;
    s_http_ok   = false;
    xTaskCreatePinnedToCoreWithCaps(http_task, "ai_http", 24 * 1024,
                                    (void *)(uintptr_t)rec_bytes, 4, &s_http_task_h,
                                    1, MALLOC_CAP_SPIRAM);
}

/* ================================================================
 *  WAV header builder (44 bytes)
 * ================================================================ */
static void build_wav_header(uint8_t *hdr, size_t pcm_bytes)
{
    uint32_t file_size = 36 + pcm_bytes;
    uint32_t byte_rate = REC_SAMPLE_RATE * 1 * 2;  /* sampleRate * channels * bytesPerSample */
    uint16_t block_align = 2;

    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4,  &file_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t chunk_size = 16;
    memcpy(hdr + 16, &chunk_size, 4);
    uint16_t fmt = 1; /* PCM */
    memcpy(hdr + 20, &fmt, 2);
    uint16_t channels = 1;
    memcpy(hdr + 22, &channels, 2);
    uint32_t sr = REC_SAMPLE_RATE;
    memcpy(hdr + 24, &sr, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    uint16_t bps = 16;
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcm_bytes, 4);
}

/* ================================================================
 *  HTTP response accumulator (used for HTTPS with chunked data)
 * ================================================================ */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (rb->len + evt->data_len < rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* ================================================================
 *  HTTP task — direct Groq STT + LLM (no relay server)
 * ================================================================ */
static void http_task(void *arg)
{
    size_t rec_bytes = (size_t)(uintptr_t)arg;

    /* ──────────────────────────────────────────────
     *  Step 1: Build multipart body for Whisper STT
     * ────────────────────────────────────────────── */
    static const char *boundary = "----ESProBoundary";

    /* WAV header */
    uint8_t wav_hdr[44];
    build_wav_header(wav_hdr, rec_bytes);

    /* Multipart preamble (model field + file header) */
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

    /* Multipart epilogue */
    char epilogue[64];
    int epi_len = snprintf(epilogue, sizeof(epilogue),
        "\r\n--%s--\r\n", boundary);

    size_t total_len = pre_len + 44 + rec_bytes + epi_len;

    ESP_LOGI(TAG, "STT: sending %u bytes audio (%u total multipart)", (unsigned)rec_bytes, (unsigned)total_len);

    /* Response buffer for JSON */
    char *json_buf = heap_caps_calloc(1, MAX_JSON_RESP, MALLOC_CAP_SPIRAM);
    if (!json_buf) {
        snprintf(s_error, sizeof(s_error), "No memory for response");
        goto done;
    }
    resp_buf_t rb = { .buf = json_buf, .len = 0, .cap = MAX_JSON_RESP - 1 };

    /* Auth header */
    char auth_hdr[128];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", GROQ_API_KEY);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    /* ── STT HTTP request ── */
    esp_http_client_config_t stt_cfg = {
        .url                = GROQ_STT_URL,
        .method             = HTTP_METHOD_POST,
        .timeout_ms         = 60000,
        .buffer_size        = 4096,
        .buffer_size_tx     = 4096,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .event_handler      = http_event_handler,
        .user_data          = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&stt_cfg);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", content_type);

    /* Check WiFi before attempting HTTPS */
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "STT: WiFi not connected");
        snprintf(s_error, sizeof(s_error), "No WiFi connection");
        esp_http_client_cleanup(client);
        goto done;
    }

    /* Heap diagnostics before TLS handshake */
    ESP_LOGI(TAG, "Heap before STT connect: internal=%u free, PSRAM=%u free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = esp_http_client_open(client, total_len);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "STT connect attempt %d failed: %s", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT connection failed after retries: %s", esp_err_to_name(err));
        snprintf(s_error, sizeof(s_error), "STT connect failed (%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto done;
    }

    /* Send preamble */
    esp_http_client_write(client, preamble, pre_len);
    /* Send WAV header */
    esp_http_client_write(client, (char *)wav_hdr, 44);
    /* Send PCM data in chunks */
    size_t sent = 0;
    while (sent < rec_bytes && !s_abort) {
        size_t chunk = rec_bytes - sent;
        if (chunk > 4096) chunk = 4096;
        int w = esp_http_client_write(client, (char *)s_rec_buf + sent, chunk);
        if (w < 0) { snprintf(s_error, sizeof(s_error), "STT send error"); esp_http_client_close(client); esp_http_client_cleanup(client); goto done; }
        sent += w;
    }
    /* Send epilogue */
    esp_http_client_write(client, epilogue, epi_len);

    /* Free recording buffer */
    free(s_rec_buf); s_rec_buf = NULL;

    if (s_abort) { esp_http_client_close(client); esp_http_client_cleanup(client); goto done; }

    /* Read STT response */
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Read remaining data */
    while (1) {
        int n = esp_http_client_read(client, json_buf + rb.len, MAX_JSON_RESP - 1 - rb.len);
        if (n <= 0) break;
        rb.len += n;
    }
    json_buf[rb.len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "STT HTTP %d, response: %.200s", status, json_buf);

    if (status != 200) {
        snprintf(s_error, sizeof(s_error), "STT error (%d)", status);
        goto done;
    }

    /* Parse transcript */
    cJSON *stt_root = cJSON_Parse(json_buf);
    if (!stt_root) {
        snprintf(s_error, sizeof(s_error), "STT JSON parse error");
        goto done;
    }
    cJSON *text_j = cJSON_GetObjectItem(stt_root, "text");
    if (text_j && cJSON_IsString(text_j) && text_j->valuestring) {
        strncpy(s_transcript, text_j->valuestring, sizeof(s_transcript) - 1);
        s_transcript[sizeof(s_transcript) - 1] = '\0';
    } else {
        strncpy(s_transcript, "(no transcript)", sizeof(s_transcript) - 1);
    }
    cJSON_Delete(stt_root);

    ESP_LOGI(TAG, "[STT] %s", s_transcript);

    if (s_abort) goto done;

    /* ──────────────────────────────────────────────
     *  Step 2: Call Groq LLM (Chat Completions)
     * ────────────────────────────────────────────── */
    ESP_LOGI(TAG, "LLM: sending prompt...");

    /* Build JSON request */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", GROQ_LLM_MODEL);
    cJSON_AddNumberToObject(req, "max_tokens", 150);
    cJSON_AddNumberToObject(req, "temperature", 0.7);
    cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", SYSTEM_PROMPT);
    cJSON_AddItemToArray(msgs, sys_msg);
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", s_transcript);
    cJSON_AddItemToArray(msgs, usr_msg);

    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        snprintf(s_error, sizeof(s_error), "JSON build failed");
        goto done;
    }
    size_t req_len = strlen(req_str);

    /* Reset response buffer */
    rb.len = 0;
    memset(json_buf, 0, MAX_JSON_RESP);

    esp_http_client_config_t llm_cfg = {
        .url                = GROQ_CHAT_URL,
        .method             = HTTP_METHOD_POST,
        .timeout_ms         = 60000,
        .buffer_size        = 4096,
        .buffer_size_tx     = 4096,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .event_handler      = http_event_handler,
        .user_data          = &rb,
    };
    client = esp_http_client_init(&llm_cfg);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    err = esp_http_client_open(client, req_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM connection failed: %s", esp_err_to_name(err));
        snprintf(s_error, sizeof(s_error), "LLM connect failed");
        free(req_str);
        esp_http_client_cleanup(client);
        goto done;
    }

    esp_http_client_write(client, req_str, req_len);
    free(req_str);

    if (s_abort) { esp_http_client_close(client); esp_http_client_cleanup(client); goto done; }

    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    while (1) {
        int n = esp_http_client_read(client, json_buf + rb.len, MAX_JSON_RESP - 1 - rb.len);
        if (n <= 0) break;
        rb.len += n;
    }
    json_buf[rb.len] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "LLM HTTP %d, response: %.300s", status, json_buf);

    if (status != 200) {
        snprintf(s_error, sizeof(s_error), "LLM error (%d)", status);
        goto done;
    }

    /* Parse LLM response */
    cJSON *llm_root = cJSON_Parse(json_buf);
    if (!llm_root) {
        snprintf(s_error, sizeof(s_error), "LLM JSON parse error");
        goto done;
    }
    cJSON *choices = cJSON_GetObjectItem(llm_root, "choices");
    if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first, "message");
        if (message) {
            cJSON *content = cJSON_GetObjectItem(message, "content");
            if (content && cJSON_IsString(content) && content->valuestring) {
                strncpy(s_response, content->valuestring, sizeof(s_response) - 1);
                s_response[sizeof(s_response) - 1] = '\0';
            }
        }
    }
    cJSON_Delete(llm_root);

    ESP_LOGI(TAG, "[LLM] %s", s_response);

    if (s_response[0]) {
        s_http_ok = true;
    } else {
        snprintf(s_error, sizeof(s_error), "Empty LLM response");
    }

    /* ──────────────────────────────────────────────
     *  Step 3: TTS — synthesize & play speech via Groq
     * ────────────────────────────────────────────── */
    if (s_http_ok && !s_abort) {
        /* Free JSON buffer early — not needed anymore */
        free(json_buf); json_buf = NULL;
        /* Signal UI to show text right away */
        s_http_done = true;

        ESP_LOGI(TAG, "TTS: synthesizing speech...");

        cJSON *tts_req = cJSON_CreateObject();
        cJSON_AddStringToObject(tts_req, "model", GROQ_TTS_MODEL);
        cJSON_AddStringToObject(tts_req, "input", s_response);
        cJSON_AddStringToObject(tts_req, "voice", GROQ_TTS_VOICE);
        cJSON_AddStringToObject(tts_req, "response_format", "wav");
        char *tts_body = cJSON_PrintUnformatted(tts_req);
        cJSON_Delete(tts_req);

        if (!tts_body) goto done;
        size_t tts_body_len = strlen(tts_body);

        uint8_t *audio_buf = heap_caps_malloc(TTS_MAX_AUDIO, MALLOC_CAP_SPIRAM);
        if (!audio_buf) {
            ESP_LOGE(TAG, "TTS: audio buffer alloc failed");
            free(tts_body);
            goto done;
        }

        esp_http_client_config_t tts_cfg = {
            .url               = GROQ_TTS_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 30000,
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t tts_client = esp_http_client_init(&tts_cfg);
        esp_http_client_set_header(tts_client, "Authorization", auth_hdr);
        esp_http_client_set_header(tts_client, "Content-Type", "application/json");

        err = esp_http_client_open(tts_client, tts_body_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TTS: connect failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(tts_client);
            free(tts_body);
            heap_caps_free(audio_buf);
            goto done;
        }

        esp_http_client_write(tts_client, tts_body, tts_body_len);
        free(tts_body);

        esp_http_client_fetch_headers(tts_client);
        int tts_status = esp_http_client_get_status_code(tts_client);

        size_t audio_len = 0;
        if (tts_status == 200) {
            while (audio_len < TTS_MAX_AUDIO && !s_abort) {
                int n = esp_http_client_read(tts_client,
                            (char *)audio_buf + audio_len,
                            TTS_MAX_AUDIO - audio_len);
                if (n <= 0) break;
                audio_len += n;
            }
            ESP_LOGI(TAG, "TTS: received %u bytes audio", (unsigned)audio_len);
        } else {
            ESP_LOGE(TAG, "TTS: HTTP %d", tts_status);
        }

        esp_http_client_close(tts_client);
        esp_http_client_cleanup(tts_client);

        /* ── Play WAV audio through ES8311 DAC ── */
        if (audio_len > 44 && !s_abort) {
            uint32_t wav_sr;
            uint16_t wav_ch, wav_bps;
            memcpy(&wav_sr,  audio_buf + 24, 4);
            memcpy(&wav_ch,  audio_buf + 22, 2);
            memcpy(&wav_bps, audio_buf + 34, 2);
            ESP_LOGI(TAG, "TTS WAV: %luHz %uch %ubps",
                     (unsigned long)wav_sr, wav_ch, wav_bps);

            /* Find "data" chunk in WAV */
            size_t data_off = 12;
            size_t data_size = 0;
            while (data_off + 8 <= audio_len) {
                uint32_t chunk_sz;
                memcpy(&chunk_sz, audio_buf + data_off + 4, 4);
                if (memcmp(audio_buf + data_off, "data", 4) == 0) {
                    data_off += 8;
                    data_size = chunk_sz;
                    if (data_off + data_size > audio_len)
                        data_size = audio_len - data_off;
                    break;
                }
                data_off += 8 + chunk_sz;
                if (chunk_sz & 1) data_off++;  /* padding byte */
            }

            if (data_size > 0 && !s_abort) {
                /* Enable PA */
                if (io_expander_handle)
                    esp_io_expander_set_level(io_expander_handle,
                                              TCA9554_PA_PIN_BIT, 1);
                vTaskDelay(pdMS_TO_TICKS(50));

                i2s_chan_handle_t tx = mic_get_shared_i2s_tx();
                if (tx) {
                    /* Explicitly reconfigure I2S TX for TTS sample rate */
                    i2s_channel_disable(tx);
                    i2s_std_clk_config_t clk_cfg =
                        I2S_STD_CLK_DEFAULT_CONFIG(wav_sr);
                    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
                    i2s_channel_reconfig_std_clock(tx, &clk_cfg);
                    i2s_std_slot_config_t slot_cfg =
                        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
                    i2s_channel_reconfig_std_slot(tx, &slot_cfg);
                    i2s_channel_enable(tx);

                    /* Set up ES8311 codec for TTS sample rate */
                    audio_codec_i2c_cfg_t i2c_cfg = {
                        .addr       = ES8311_CODEC_DEFAULT_ADDR,
                        .bus_handle = i2c_port0_bus_handle,
                    };
                    const audio_codec_ctrl_if_t *ctrl =
                        audio_codec_new_i2c_ctrl(&i2c_cfg);
                    const audio_codec_gpio_if_t *gpio =
                        audio_codec_new_gpio();

                    es8311_codec_cfg_t es_cfg = {
                        .ctrl_if    = ctrl,
                        .gpio_if    = gpio,
                        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                        .pa_pin     = -1,
                        .use_mclk   = true,
                        .mclk_div   = 256,
                    };
                    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);

                    if (codec) {
                        audio_codec_i2s_cfg_t data_cfg = {
                            .port = 0, .tx_handle = tx,
                        };
                        const audio_codec_data_if_t *data =
                            audio_codec_new_i2s_data(&data_cfg);

                        esp_codec_dev_cfg_t dev_cfg = {
                            .codec_if = codec, .data_if = data,
                            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
                        };
                        esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);

                        esp_codec_dev_sample_info_t fs = {
                            .bits_per_sample = 16,
                            .channel         = 1,
                            .sample_rate     = wav_sr,
                            .mclk_multiple   = 256,
                        };
                        esp_codec_dev_open(dev, &fs);
                        esp_codec_dev_set_out_vol(dev, g_volume * 100 / 255);

                        /* Stereo → mono downmix in-place if needed */
                        uint8_t *pcm = audio_buf + data_off;
                        size_t pcm_len = data_size;

                        if (wav_ch == 2 && wav_bps == 16) {
                            size_t mono_samples = pcm_len / 4;
                            int16_t *src = (int16_t *)pcm;
                            for (size_t i = 0; i < mono_samples; i++)
                                src[i] = (int16_t)(((int32_t)src[2*i]
                                                   + src[2*i+1]) / 2);
                            pcm_len = mono_samples * 2;
                        }

                        /* Write PCM in chunks */
                        size_t pos = 0;
                        while (pos < pcm_len && !s_abort) {
                            size_t to_write = pcm_len - pos;
                            if (to_write > 4096) to_write = 4096;
                            size_t written;
                            i2s_channel_write(tx, pcm + pos, to_write,
                                              &written, pdMS_TO_TICKS(500));
                            pos += written;
                        }

                        /* Flush with silence */
                        uint8_t silence[512] = {0};
                        size_t written;
                        i2s_channel_write(tx, silence, sizeof(silence),
                                          &written, pdMS_TO_TICKS(100));

                        esp_codec_dev_close(dev);
                        esp_codec_dev_delete(dev);
                    }
                    /* Disable TX after TTS playback */
                    i2s_channel_disable(tx);
                }
            }
        }
        heap_caps_free(audio_buf);
    }

done:
    if (json_buf) free(json_buf);
    if (s_rec_buf) { free(s_rec_buf); s_rec_buf = NULL; }
    s_http_done = true;
    s_http_task_h = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  State transitions
 * ================================================================ */
static void set_state(assist_state_t st)
{
    s_state = st;
    if (st == AST_PROCESSING) {
        s_processing_start = (uint32_t)(esp_timer_get_time() / 1000);
    }
    update_button_style();
}

/* ================================================================
 *  Button callback
 * ================================================================ */
static void siri_btn_cb(lv_event_t *e)
{
    (void)e;
    switch (s_state) {
        case AST_IDLE:
        case AST_SPEAKING:  /* speaking just means "showing result" — allow re-ask */
            s_transcript[0] = '\0';
            s_response[0]   = '\0';
            s_error[0]      = '\0';
            if (s_transcript_lbl) lv_label_set_text(s_transcript_lbl, "");
            if (s_response_lbl)   lv_label_set_text(s_response_lbl, "");
            start_listening();
            break;

        case AST_LISTENING:
            stop_listening();
            break;

        case AST_PROCESSING:
            /* Allow abort — signal task and force reset */
            ESP_LOGW(TAG, "User abort during processing");
            s_abort = true;
            /* Give the task 2s to finish, then force reset */
            for (int i = 0; i < 20 && s_http_task_h; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (s_http_task_h) {
                ESP_LOGW(TAG, "Force-deleting hung HTTP task");
                vTaskDelete(s_http_task_h);
                s_http_task_h = NULL;
            }
            free_buffers();
            s_http_done = false;
            s_http_ok   = false;
            s_abort     = false;
            if (s_response_lbl)
                lv_label_set_text(s_response_lbl, "Cancelled");
            set_state(AST_IDLE);
            break;
    }
}

/* ================================================================
 *  Back button
 * ================================================================ */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    screen_assistant_close();
}

/* ================================================================
 *  Open / Close / Update
 * ================================================================ */
void screen_assistant_open(lv_obj_t *parent)
{
    if (s_active) return;

    /* Close music player if active */
    if (music_player_is_active()) {
        music_player_close();
    }

    s_active = true;
    s_abort  = false;
    s_state  = AST_IDLE;
    s_transcript[0] = '\0';
    s_response[0]   = '\0';
    s_error[0]      = '\0';
    memset(s_wave_levels, 0, sizeof(s_wave_levels));

    /* ── Overlay ── */
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(th_bg), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Back button ── */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 50, 24);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 3);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* ── Status label (top centre) ── */
    s_status_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_status_lbl, "Tap to speak");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 30, 6);

    /* ── Siri ring (outer glow) ── */
    s_siri_ring = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_siri_ring);
    lv_obj_set_size(s_siri_ring, RING_SIZE, RING_SIZE);
    lv_obj_set_pos(s_siri_ring, SIRI_CX - RING_SIZE / 2,
                   SIRI_CY - RING_SIZE / 2);
    lv_obj_set_style_bg_color(s_siri_ring, lv_color_hex(COL_RING_IDLE), 0);
    lv_obj_set_style_bg_opa(s_siri_ring, 60, 0);
    lv_obj_set_style_radius(s_siri_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_siri_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* ── Siri button ── */
    s_siri_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_siri_btn, SIRI_SIZE, SIRI_SIZE);
    lv_obj_set_pos(s_siri_btn, SIRI_CX - SIRI_SIZE / 2,
                    SIRI_CY - SIRI_SIZE / 2);
    lv_obj_set_style_radius(s_siri_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_siri_btn, lv_color_hex(COL_IDLE), 0);
    lv_obj_set_style_bg_opa(s_siri_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_siri_btn, 16, 0);
    lv_obj_set_style_shadow_color(s_siri_btn, lv_color_hex(COL_IDLE), 0);
    lv_obj_set_style_shadow_opa(s_siri_btn, LV_OPA_60, 0);
    lv_obj_add_event_cb(s_siri_btn, siri_btn_cb, LV_EVENT_CLICKED, NULL);

    /* ── Mic icon inside button ── */
    s_siri_icon = lv_label_create(s_siri_btn);
    lv_label_set_text(s_siri_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_siri_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_siri_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_siri_icon);

    /* ── Right panel: transcript ── */
    s_transcript_lbl = lv_label_create(s_overlay);
    lv_label_set_long_mode(s_transcript_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_transcript_lbl, PANEL_W);
    lv_label_set_text(s_transcript_lbl, "");
    lv_obj_set_style_text_font(s_transcript_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_transcript_lbl, lv_color_hex(0xAABBCC), 0);
    lv_obj_set_pos(s_transcript_lbl, PANEL_X, 24);

    /* ── Right panel: response (fills area between transcript and waveform) ── */
    lv_obj_t *resp_box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(resp_box);
    lv_obj_set_size(resp_box, PANEL_W, WAVE_Y - 42);   /* from y=40 to waveform */
    lv_obj_set_pos(resp_box, PANEL_X, 40);
    lv_obj_set_style_bg_color(resp_box, lv_color_hex(th_card), 0);
    lv_obj_set_style_bg_opa(resp_box, LV_OPA_70, 0);
    lv_obj_set_style_radius(resp_box, 8, 0);
    lv_obj_set_style_pad_all(resp_box, 6, 0);
    lv_obj_add_flag(resp_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(resp_box, LV_DIR_VER);

    s_response_lbl = lv_label_create(resp_box);
    lv_label_set_long_mode(s_response_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_response_lbl, PANEL_W - 14);
    lv_label_set_text(s_response_lbl, "");
    lv_obj_set_style_text_font(s_response_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_response_lbl, lv_color_hex(0xEEEEEE), 0);

    /* ── Waveform bars (thin strip at bottom) ── */
    for (int i = 0; i < WAVE_BAR_CNT; i++) {
        s_wave_bars[i] = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(s_wave_bars[i]);
        lv_obj_set_size(s_wave_bars[i], WAVE_BAR_W, 2);
        int x = PANEL_X + i * (WAVE_BAR_W + WAVE_BAR_GAP);
        lv_obj_set_pos(s_wave_bars[i], x, WAVE_Y + WAVE_MAX_H - 2);
        lv_obj_set_style_bg_color(s_wave_bars[i], lv_color_hex(COL_WAVE_IDLE), 0);
        lv_obj_set_style_bg_opa(s_wave_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wave_bars[i], 2, 0);
        lv_obj_clear_flag(s_wave_bars[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── Wave animation timer ── */
    s_wave_timer = lv_timer_create(wave_timer_cb, 60, NULL);

    ESP_LOGI(TAG, "Assistant opened");
}

void screen_assistant_close(void)
{
    if (!s_active) return;
    s_abort = true;

    /* Stop mic if recording */
    if (s_state == AST_LISTENING) {
        mic_stop();
        mic_clear_record_buffer();
    }

    /* Wait for HTTP task to finish (with timeout) */
    for (int i = 0; i < 30 && s_http_task_h; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Delete timer */
    if (s_wave_timer) { lv_timer_del(s_wave_timer); s_wave_timer = NULL; }

    /* Delete overlay */
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }

    /* Null out child pointers (deleted with overlay) */
    s_siri_btn = NULL;
    s_siri_ring = NULL;
    s_siri_icon = NULL;
    s_status_lbl = NULL;
    s_transcript_lbl = NULL;
    s_response_lbl = NULL;
    memset(s_wave_bars, 0, sizeof(s_wave_bars));

    free_buffers();

    s_state  = AST_IDLE;
    s_active = false;
    s_abort  = false;

    ESP_LOGI(TAG, "Assistant closed");
}

void screen_assistant_update(void)
{
    if (!s_active) return;

    /* ── Failsafe: timeout watchdog for PROCESSING state ── */
    if (s_state == AST_PROCESSING && !s_http_done) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now - s_processing_start) > PROCESSING_TIMEOUT_MS) {
            ESP_LOGE(TAG, "PROCESSING timeout (%d ms) — force reset",
                     PROCESSING_TIMEOUT_MS);
            s_abort = true;
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_http_task_h) {
                vTaskDelete(s_http_task_h);
                s_http_task_h = NULL;
            }
            free_buffers();
            s_http_done = false;
            s_http_ok   = false;
            s_abort     = false;
            if (s_response_lbl)
                lv_label_set_text(s_response_lbl, "Error: Request timed out");
            set_state(AST_IDLE);
            return;
        }
    }

    /* ── HTTP done → show results ── */
    if (s_http_done && s_state == AST_PROCESSING) {
        s_http_done = false;

        if (s_http_ok) {
            /* Show transcript and response text */
            if (s_transcript_lbl)
                lv_label_set_text_fmt(s_transcript_lbl,
                                      LV_SYMBOL_AUDIO "  %s", s_transcript);
            if (s_response_lbl)
                lv_label_set_text(s_response_lbl, s_response);

            /* Stay in SPEAKING state (shows blue) — tap again to re-ask */
            set_state(AST_SPEAKING);
        } else {
            /* Error */
            if (s_response_lbl) {
                lv_label_set_text_fmt(s_response_lbl,
                    "Error: %s", s_error[0] ? s_error : "Unknown");
            }
            set_state(AST_IDLE);
        }
    }
}

bool screen_assistant_is_active(void)
{
    return s_active;
}

static void free_buffers(void)
{
    if (s_rec_buf)  { free(s_rec_buf);  s_rec_buf  = NULL; }
}
