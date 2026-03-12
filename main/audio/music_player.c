/**
 * @file music_player.c
 * @brief JioSaavn music player — streams curated Indian playlists.
 *
 * Pipeline:  JioSaavn API → decrypt URL → HTTP stream → MP3 decode → I2S
 *
 * Categories: Bollywood Latest, Bollywood 90s, Bhajans, Dance Mix, Lofi.
 * Songs fetched via JioSaavn search API, URLs decrypted (DES-ECB), played as MP3.
 */

#include "music_player.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "mic_input.h"
#include "screen_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "minimp3.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

/* esp_codec_dev — ES8311 driver */
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "mbedtls/des.h"
#include "mbedtls/base64.h"

static const char *TAG = "music";

/* ================================================================
 *  JioSaavn Configuration
 * ================================================================
 *
 * Search queries per genre — edit these to change what songs appear.
 * The player searches JioSaavn for each query and plays matching songs.
 *
 * Media URL decryption key (well-known, required for JioSaavn CDN):
 */
#define JIOSAAVN_DES_KEY    "38346591"
#define JIOSAAVN_API_BASE   "https://www.jiosaavn.com/api.php"
#define JIOSAAVN_QUALITY    "160"   /* 96=low  160=medium(MP3)  320=high(AAC) */

#define MAX_STATIONS    10
#define NAME_LEN        96
#define URL_LEN         384

typedef struct {
    char name[NAME_LEN];
    char url[URL_LEN];
} station_t;

typedef struct {
    const char *label;          /* display name  */
    const char *query;          /* JioSaavn search query (URL-encoded) */
} genre_t;

/*
 * ── Curated JioSaavn search queries ──────────────────────────
 * Spaces must be '+' (URL query encoding).
 * Tweak these strings to refine the song selection.
 */
static const genre_t genres[] = {
    { "BW Latest",  "new+hindi+songs"            },
    { "BW 90s",     "90s+bollywood+hits"          },
    { "Bhajans",    "bhajans+devotional"          },
    { "Dance Mix",  "bollywood+dance+party+hits"  },
    { "Lofi",       "hindi+lofi+chill"            },
};
#define GENRE_COUNT (sizeof(genres)/sizeof(genres[0]))

/* ── Dynamic station list (fetched from API or loaded from fallbacks) ── */
static station_t s_stations[MAX_STATIONS];
static int       s_station_count    = 0;
static int       s_current_station  = 0;
static int       s_genre_idx        = 0;

/* ================================================================
 *  Player state
 * ================================================================ */
typedef enum {
    PS_IDLE, PS_CONNECTING, PS_PLAYING, PS_PAUSED, PS_ERROR
} player_state_t;

static volatile player_state_t s_state = PS_IDLE;
static bool s_active       = false;

/* Song info displayed on UI */
static char s_song_title[128]  = "---";
static char s_station_name[64] = "";
static volatile bool s_meta_updated  = false;
static volatile bool s_state_changed = false;

/* Deferred commands (set by UI callbacks, processed in update) */
static volatile bool s_close_requested   = false;
static volatile bool s_play_requested    = false;
static volatile bool s_pause_toggle      = false;
static volatile bool s_skip_next         = false;
static volatile bool s_skip_prev         = false;
static volatile int  s_genre_switch      = -1;
static volatile bool s_new_song          = false;

/* ================================================================
 *  Audio pipeline handles
 * ================================================================ */
#define STREAM_BUF_SIZE     (256 * 1024)   /* PSRAM ring buffer */
#define DEC_INPUT_BUF_SIZE  (8 * 1024)
#define PREBUFFER_BYTES     (48 * 1024)    /* fill before playback starts */
#define PCM_OUT_BUF_SIZE    (8 * 1024)

/* Backward compat — stream buffer still called s_mp3_buf internally */
#define MP3_BUF_SIZE  STREAM_BUF_SIZE

static StreamBufferHandle_t s_mp3_buf    = NULL;
static uint8_t             *s_mp3_buf_storage = NULL;   /* PSRAM backing */
static StaticStreamBuffer_t s_mp3_buf_struct;
static TaskHandle_t         s_stream_task = NULL;
static TaskHandle_t         s_decode_task = NULL;
static volatile bool        s_stop_requested = false;

static i2s_chan_handle_t         s_i2s_tx = NULL;

/* ================================================================
 *  LVGL objects
 * ================================================================ */
static lv_obj_t *s_overlay      = NULL;
static lv_obj_t *s_song_lbl     = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_play_btn_lbl = NULL;
static lv_obj_t *s_idx_lbl      = NULL;

/* ================================================================
 *  esp_codec_dev handles (ES8311 DAC)
 * ================================================================ */
static esp_codec_dev_handle_t  s_codec_dev = NULL;
static const audio_codec_if_t      *s_codec_if  = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if   = NULL;
static const audio_codec_data_if_t *s_data_if   = NULL;
static const audio_codec_gpio_if_t *s_gpio_if   = NULL;

/* ================================================================
 *  Audio hardware init / deinit  (called from decode task)
 * ================================================================ */
static void audio_init(int sample_rate)
{
    if (io_expander_handle) {
        esp_io_expander_set_level(io_expander_handle, TCA9554_PA_PIN_BIT, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    s_i2s_tx = mic_get_shared_i2s_tx();
    if (!s_i2s_tx) {
        ESP_LOGE(TAG, "No shared I2S TX handle");
        return;
    }

    /* ── Explicitly reconfigure I2S TX channel to the target sample rate.
     *    This guarantees correct clocks regardless of codec_dev
     *    pairing/keeper state from mic or boot_sound. ── */
    i2s_channel_disable(s_i2s_tx);

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t rc = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "I2S clock reconfig to %d Hz failed: %s",
                 sample_rate, esp_err_to_name(rc));
    }

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    rc = i2s_channel_reconfig_std_slot(s_i2s_tx, &slot_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "I2S slot reconfig failed: %s", esp_err_to_name(rc));
    }

    i2s_channel_enable(s_i2s_tx);

    /* ── ES8311 codec setup via esp_codec_dev ── */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_port0_bus_handle,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = s_ctrl_if,
        .gpio_if    = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,
        .use_mclk   = true,
        .mclk_div   = 256,
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (!s_codec_if) {
        ESP_LOGE(TAG, "ES8311 codec new failed");
        return;
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port      = 0,
        .tx_handle = s_i2s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = (uint32_t)sample_rate,
        .mclk_multiple   = 256,
    };
    int ret = esp_codec_dev_open(s_codec_dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d (sr=%d)", ret, sample_rate);
    }
    esp_codec_dev_set_out_vol(s_codec_dev, g_volume * 100 / 255);

    ESP_LOGI(TAG, "Audio init: %d Hz mono (vol=%d)", sample_rate, g_volume);
}

static void audio_deinit(void)
{
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    /* Disable TX but don't delete (shared channel) */
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
    }
    s_i2s_tx   = NULL;
    s_codec_if = NULL;
    s_ctrl_if  = NULL;
    s_data_if  = NULL;
    s_gpio_if  = NULL;
}

/* ================================================================
 *  JioSaavn URL decryption (DES-ECB + Base64)
 * ================================================================ */
static int jiosaavn_decrypt_url(const char *encrypted, char *out, size_t out_size)
{
    size_t enc_len = strlen(encrypted);
    if (enc_len == 0 || enc_len > 1024) return -1;

    /* Base64 decode */
    size_t dec_len = 0;
    unsigned char *decoded = heap_caps_malloc(enc_len, MALLOC_CAP_SPIRAM);
    if (!decoded) return -1;

    int ret = mbedtls_base64_decode(decoded, enc_len, &dec_len,
                                    (const unsigned char *)encrypted, enc_len);
    if (ret != 0 || dec_len == 0 || dec_len % 8 != 0) {
        ESP_LOGW(TAG, "Base64 decode failed (ret=%d len=%d)", ret, (int)dec_len);
        heap_caps_free(decoded);
        return -1;
    }

    if (dec_len + 1 > out_size) {
        heap_caps_free(decoded);
        return -1;
    }

    /* DES-ECB decrypt — 8 bytes at a time */
    mbedtls_des_context ctx;
    mbedtls_des_init(&ctx);
    mbedtls_des_setkey_dec(&ctx, (const unsigned char *)JIOSAAVN_DES_KEY);

    for (size_t i = 0; i + 8 <= dec_len; i += 8) {
        mbedtls_des_crypt_ecb(&ctx, decoded + i, (unsigned char *)out + i);
    }
    mbedtls_des_free(&ctx);
    heap_caps_free(decoded);

    /* Remove PKCS#5 padding */
    int pad = (unsigned char)out[dec_len - 1];
    if (pad >= 1 && pad <= 8) dec_len -= pad;
    out[dec_len] = '\0';

    /* Keep .mp4 extension — CDN serves AAC in M4A container
     * We'll use esp_audio_codec M4A simple decoder for playback */

    return 0;
}

/* ================================================================
 *  Fetch songs from JioSaavn search API
 * ================================================================ */
static int fetch_jiosaavn_songs(int genre)
{
    const genre_t *g = &genres[genre];

    char url[512];
    snprintf(url, sizeof(url),
             JIOSAAVN_API_BASE
             "?__call=search.getResults&p=1&q=%s"
             "&_format=json&_marker=0&n=%d",
             g->query, MAX_STATIONS);

    ESP_LOGI(TAG, "JioSaavn: searching '%s'", g->label);

    #define API_BUF_SIZE  (48 * 1024)
    char *buf = heap_caps_malloc(API_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "API buf alloc failed");
        return -1;
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 15000,
        .buffer_size       = 16384,
        .buffer_size_tx    = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent",
                               "Mozilla/5.0 (Linux; Android 14) ESPro/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "API connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(buf);
        return -1;
    }

    esp_http_client_fetch_headers(client);
    int total = 0, n;
    while ((n = esp_http_client_read(client, buf + total,
                                     API_BUF_SIZE - 1 - total)) > 0) {
        total += n;
        if (total >= API_BUF_SIZE - 1) break;
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "API response: %d bytes (buf %d)", total, API_BUF_SIZE);
    if (total <= 0) {
        ESP_LOGW(TAG, "Empty API response");
        heap_caps_free(buf);
        return -1;
    }
    if (total >= API_BUF_SIZE - 2) {
        ESP_LOGW(TAG, "API response truncated (%d >= %d)", total, API_BUF_SIZE);
    }

    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return -1;
    }

    /* search.getResults returns { "results": [ ... ] } */
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results)) {
        ESP_LOGW(TAG, "No results array");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(results);
    if (count > MAX_STATIONS) count = MAX_STATIONS;

    char dec_url[URL_LEN];
    int loaded = 0;

    for (int i = 0; i < count && loaded < MAX_STATIONS; i++) {
        cJSON *item = cJSON_GetArrayItem(results, i);
        if (!item) continue;

        /* Song title — try "song" then "title" */
        cJSON *j_title = cJSON_GetObjectItem(item, "song");
        if (!j_title) j_title = cJSON_GetObjectItem(item, "title");

        /* Artist — try "singers" then "primary_artists" */
        cJSON *j_artist = cJSON_GetObjectItem(item, "singers");
        if (!j_artist) j_artist = cJSON_GetObjectItem(item, "primary_artists");

        /* Encrypted URL — top-level or inside more_info */
        cJSON *j_enc = cJSON_GetObjectItem(item, "encrypted_media_url");
        if (!j_enc || !j_enc->valuestring) {
            cJSON *mi = cJSON_GetObjectItem(item, "more_info");
            if (mi) j_enc = cJSON_GetObjectItem(mi, "encrypted_media_url");
        }

        if (!j_title || !j_enc) continue;
        if (!j_title->valuestring || !j_enc->valuestring) continue;
        if (strlen(j_enc->valuestring) < 10) continue;

        /* Decrypt media URL */
        if (jiosaavn_decrypt_url(j_enc->valuestring, dec_url,
                                 sizeof(dec_url)) != 0) {
            ESP_LOGW(TAG, "Decrypt failed for song %d", i);
            continue;
        }

        /* Build display name: "Title - Artist" */
        if (j_artist && j_artist->valuestring && strlen(j_artist->valuestring) > 0) {
            snprintf(s_stations[loaded].name, NAME_LEN, "%s - %s",
                     j_title->valuestring, j_artist->valuestring);
        } else {
            strncpy(s_stations[loaded].name, j_title->valuestring, NAME_LEN - 1);
            s_stations[loaded].name[NAME_LEN - 1] = '\0';
        }

        strncpy(s_stations[loaded].url, dec_url, URL_LEN - 1);
        s_stations[loaded].url[URL_LEN - 1] = '\0';

        loaded++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d songs for %s", loaded, g->label);
    return loaded;
}

static void load_stations(int genre)
{
    s_station_count   = 0;
    s_current_station = 0;

    int fetched = fetch_jiosaavn_songs(genre);
    if (fetched > 0) {
        s_station_count = fetched;
        /* Shuffle for variety */
        for (int i = s_station_count - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            station_t tmp = s_stations[i];
            s_stations[i] = s_stations[j];
            s_stations[j] = tmp;
        }
    }
}

/* ================================================================
 *  Stream internet radio (continuous HTTP MP3 stream)
 *  Handles redirects and ICY streams.
 * ================================================================ */
static void stream_radio(const char *url)
{
    ESP_LOGI(TAG, "Radio: connecting to %.120s", url);

    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 15000,
        .buffer_size    = 16384,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "ESPro-Radio/1.0");
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Radio connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Follow HTTP redirects manually (open/read API doesn't auto-redirect) */
    int redirects = 0;
    while ((status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) && redirects < 5) {
        redirects++;
        ESP_LOGI(TAG, "HTTP %d redirect #%d", status, redirects);
        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) break;
        esp_http_client_close(client);
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) break;
        content_len = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }

    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "Radio HTTP %d (after %d redirects)", status, redirects);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    /* Check for ICY metadata interval — not used for now,
     * just consume the stream data directly */

    s_state = PS_PLAYING;
    s_state_changed = true;

    ESP_LOGI(TAG, "Stream connected (HTTP %d), pumping data", status);

    uint8_t buf[4096];
    int total_bytes = 0;

    while (!s_stop_requested) {
        if (s_state == PS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_skip_next || s_skip_prev) break;

        int n = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (n <= 0) {
            ESP_LOGW(TAG, "Radio stream ended/error (n=%d), total=%d bytes", n, total_bytes);
            break;
        }
        total_bytes += n;
        if (total_bytes <= n)
            ESP_LOGI(TAG, "First data chunk: %d bytes, header: %02X %02X %02X %02X",
                     n, buf[0], buf[1], buf[2], buf[3]);
        xStreamBufferSend(s_mp3_buf, buf, n, pdMS_TO_TICKS(500));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* ================================================================
 *  Streaming task — connects to current station and streams
 * ================================================================ */
static void stream_task(void *arg)
{
    /* Fetch songs here (not in AppMgr task) — HTTPS/TLS needs 10+KB stack */
    load_stations(s_genre_idx);

    if (s_station_count == 0) {
        strncpy(s_song_title, "No songs found", sizeof(s_song_title) - 1);
        s_meta_updated  = true;
        s_state         = PS_ERROR;
        s_state_changed = true;
        ESP_LOGW(TAG, "No songs fetched");
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;  /* unreachable, but clear intent */
    }

    while (!s_stop_requested) {
        if (s_current_station >= s_station_count) {
            ESP_LOGI(TAG, "Playlist ended, looping");
            s_current_station = 0;
        }

        station_t *st = &s_stations[s_current_station];
        snprintf(s_station_name, sizeof(s_station_name), "%s",
                 genres[s_genre_idx].label);
        snprintf(s_song_title, sizeof(s_song_title), LV_SYMBOL_AUDIO " %s",
                 st->name);
        s_meta_updated  = true;
        s_state         = PS_CONNECTING;
        s_state_changed = true;

        stream_radio(st->url);

        if (s_stop_requested) break;

        /* Handle skip */
        if (s_skip_next) {
            s_skip_next = false;
            s_current_station = (s_current_station + 1) % s_station_count;
            xStreamBufferReset(s_mp3_buf);
            s_new_song = true;
            continue;
        }
        if (s_skip_prev) {
            s_skip_prev = false;
            s_current_station = (s_current_station + s_station_count - 1) % s_station_count;
            xStreamBufferReset(s_mp3_buf);
            s_new_song = true;
            continue;
        }

        /* Song ended — advance to next */
        ESP_LOGI(TAG, "Song ended, playing next");
        s_current_station = (s_current_station + 1) % s_station_count;
        xStreamBufferReset(s_mp3_buf);
        s_new_song = true;
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!s_stop_requested) {
        strncpy(s_song_title, "No songs available", sizeof(s_song_title) - 1);
        s_meta_updated  = true;
        s_state         = PS_ERROR;
        s_state_changed = true;
    }

    ESP_LOGI(TAG, "Stream task exit");
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  M4A/AAC decode + I2S output task (using esp_audio_codec)
 * ================================================================ */
static void decode_task(void *arg)
{
    ESP_LOGI(TAG, "Decode task entered (stack free: %d)",
             uxTaskGetStackHighWaterMark(NULL));

    bool hw_ok = false;
    uint8_t *in_buf  = NULL;
    uint8_t *pcm_buf = NULL;
    int16_t *mono    = NULL;
    esp_audio_simple_dec_handle_t dec = NULL;

    if (!s_mp3_buf) {
        ESP_LOGE(TAG, "Decode: stream buffer is NULL!");
        goto done;
    }

    in_buf  = heap_caps_malloc(DEC_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    pcm_buf = heap_caps_malloc(PCM_OUT_BUF_SIZE,   MALLOC_CAP_SPIRAM);
    mono    = heap_caps_malloc(PCM_OUT_BUF_SIZE,    MALLOC_CAP_SPIRAM);
    if (!in_buf || !pcm_buf || !mono) {
        ESP_LOGE(TAG, "Decode buf alloc failed");
        goto done;
    }

    /* Register M4A/AAC decoders (one-time) */
    static bool s_dec_registered = false;
    if (!s_dec_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_dec_registered = true;
    }

    /* Open M4A simple decoder — handles MP4 container + AAC decoding */
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,
    };
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &dec);
    if (ret != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "M4A decoder open failed: %d", ret);
        goto done;
    }

    ESP_LOGI(TAG, "Decode task started (M4A/AAC)");

    /* Pre-buffer: wait until stream buffer has enough data */
    ESP_LOGI(TAG, "Waiting for pre-buffer (%d KB)...", PREBUFFER_BYTES / 1024);
    while (!s_stop_requested) {
        size_t avail = xStreamBufferBytesAvailable(s_mp3_buf);
        if (avail >= PREBUFFER_BYTES) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_stop_requested) goto done;
    ESP_LOGI(TAG, "Pre-buffer ready, starting decode");

    int fill = 0;

    while (!s_stop_requested) {
        if (s_state == PS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Reset decoder between songs (new M4A container) */
        if (s_new_song) {
            s_new_song = false;

            /* Silence output immediately — stop I2S so stale PCM isn't heard */
            if (hw_ok && s_i2s_tx) {
                i2s_channel_disable(s_i2s_tx);
            }

            esp_audio_simple_dec_reset(dec);
            fill = 0;
            ESP_LOGI(TAG, "Decoder reset for new song — I2S silenced");

            /* Wait for new song data to arrive before decoding */
            while (!s_stop_requested && !s_new_song) {
                size_t avail = xStreamBufferBytesAvailable(s_mp3_buf);
                if (avail >= PREBUFFER_BYTES) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            /* Re-enable I2S once data is ready */
            if (hw_ok && s_i2s_tx) {
                i2s_channel_enable(s_i2s_tx);
            }
            if (s_stop_requested) goto done;
            continue;   /* re-check flags at top of loop */
        }

        /* Fill input buffer from stream */
        int space = DEC_INPUT_BUF_SIZE - fill;
        if (space > 0) {
            size_t got = xStreamBufferReceive(s_mp3_buf,
                            in_buf + fill, space, pdMS_TO_TICKS(100));
            fill += (int)got;
        }
        if (fill == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Feed data to M4A simple decoder */
        esp_audio_simple_dec_raw_t raw = {
            .buffer   = in_buf,
            .len      = (uint32_t)fill,
            .eos      = false,
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t frame = {
            .buffer = pcm_buf,
            .len    = PCM_OUT_BUF_SIZE,
        };

        ret = esp_audio_simple_dec_process(dec, &raw, &frame);

        /* Remove consumed data */
        if (raw.consumed > 0) {
            fill -= (int)raw.consumed;
            if (fill > 0)
                memmove(in_buf, in_buf + raw.consumed, fill);
        }

        if (ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
            if (!hw_ok) {
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                int sr = info.sample_rate > 0 ? (int)info.sample_rate : 44100;
                ESP_LOGI(TAG, "First AAC frame: %dHz %dch %dbps",
                         sr, info.channel, info.bits_per_sample);
                audio_init(sr);
                hw_ok = true;
            }

            /* Downmix stereo to mono for single-speaker output */
            esp_audio_simple_dec_info_t info = {0};
            esp_audio_simple_dec_get_info(dec, &info);
            int16_t *out = (int16_t *)pcm_buf;
            int out_bytes = (int)frame.decoded_size;

            if (info.channel == 2) {
                int samples = out_bytes / (2 * sizeof(int16_t));
                int16_t *src = (int16_t *)pcm_buf;
                for (int i = 0; i < samples; i++)
                    mono[i] = (int16_t)(((int32_t)src[2*i] + src[2*i+1]) / 2);
                out = mono;
                out_bytes = samples * sizeof(int16_t);
            }

            size_t written;
            i2s_channel_write(s_i2s_tx, out, out_bytes,
                              &written, pdMS_TO_TICKS(500));
        } else if (ret != ESP_AUDIO_ERR_OK && raw.consumed == 0 && fill > 0) {
            /* Decoder can't make progress — skip a byte to recover */
            fill--;
            if (fill > 0) memmove(in_buf, in_buf + 1, fill);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

done:
    if (hw_ok) audio_deinit();
    if (dec) esp_audio_simple_dec_close(dec);
    heap_caps_free(in_buf);
    heap_caps_free(pcm_buf);
    heap_caps_free(mono);

    ESP_LOGI(TAG, "Decode task exit");
    s_decode_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  Start / stop helpers
 * ================================================================ */
static void do_start_playback(void)
{
    s_stop_requested = false;
    s_skip_next      = false;
    s_skip_prev      = false;

    /* Stop mic if active — avoids I2S full-duplex clock conflict */
    if (mic_is_active()) {
        ESP_LOGI(TAG, "Stopping mic before playback");
        mic_stop();
    }

    /* Show loading message — actual fetch happens in stream_task (needs TLS stack) */
    snprintf(s_song_title, sizeof(s_song_title), "Loading %s...",
             genres[s_genre_idx].label);
    s_meta_updated = true;
    s_state        = PS_CONNECTING;
    s_state_changed = true;

    if (!s_mp3_buf) {
        s_mp3_buf_storage = heap_caps_malloc(MP3_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
        if (!s_mp3_buf_storage) {
            ESP_LOGE(TAG, "Stream buffer storage alloc failed (%d bytes)!", MP3_BUF_SIZE);
            s_state = PS_ERROR;
            s_state_changed = true;
            return;
        }
        s_mp3_buf = xStreamBufferCreateStatic(MP3_BUF_SIZE, 1,
                        s_mp3_buf_storage, &s_mp3_buf_struct);
        if (!s_mp3_buf) {
            ESP_LOGE(TAG, "Stream buffer create failed!");
            heap_caps_free(s_mp3_buf_storage);
            s_mp3_buf_storage = NULL;
            s_state = PS_ERROR;
            s_state_changed = true;
            return;
        }
    } else {
        xStreamBufferReset(s_mp3_buf);
    }

    BaseType_t r1 = xTaskCreatePinnedToCoreWithCaps(stream_task, "radio_rx", 24576,
                            NULL, 3, &s_stream_task, 1, MALLOC_CAP_SPIRAM);
    BaseType_t r2 = xTaskCreatePinnedToCoreWithCaps(decode_task, "mp3_dec",  32768,
                            NULL, 4, &s_decode_task, 1, MALLOC_CAP_SPIRAM);
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "Task create failed! stream=%d decode=%d", r1, r2);
        s_stop_requested = true;
        s_state = PS_ERROR;
        s_state_changed = true;
    }
}

/* ================================================================
 *  UI callbacks  (run in LVGL context — must be non-blocking)
 * ================================================================ */
static void back_cb(lv_event_t *e)   { (void)e; s_close_requested = true; }

static void play_pause_cb(lv_event_t *e)
{
    (void)e;
    if (s_state == PS_IDLE || s_state == PS_ERROR)
        s_play_requested = true;
    else
        s_pause_toggle = true;
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    s_skip_next = true;
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    s_skip_prev = true;
}

static void genre_next_cb(lv_event_t *e)
{
    (void)e;
    s_genre_switch = (s_genre_idx + 1) % GENRE_COUNT;
}

static void genre_prev_cb(lv_event_t *e)
{
    (void)e;
    s_genre_switch = (s_genre_idx + GENRE_COUNT - 1) % GENRE_COUNT;
}

static lv_obj_t *s_vol_lbl = NULL;

static void vol_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    g_volume = (uint8_t)lv_slider_get_value(slider);
    music_player_set_volume(g_volume);
    if (s_vol_lbl)
        lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%",
                              g_volume * 100 / 255);
}

static void vol_down_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)g_volume - 25;
    if (v < 0) v = 0;
    g_volume = (uint8_t)v;
    music_player_set_volume(g_volume);
    if (s_vol_lbl)
        lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%",
                              g_volume * 100 / 255);
}

static void vol_up_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)g_volume + 25;
    if (v > 255) v = 255;
    g_volume = (uint8_t)v;
    music_player_set_volume(g_volume);
    if (s_vol_lbl)
        lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%",
                              g_volume * 100 / 255);
}

/* ================================================================
 *  Build overlay UI  (640x172) — Internet Radio
 * ================================================================ */
static lv_obj_t *s_genre_lbl = NULL;

static void build_ui(void)
{
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(th_bg), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);

    /* ── Back button (top-left) ──── */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(th_text), 0);
    lv_obj_center(bl);

    /* ── Genre selector row (top) ──── */
    lv_obj_t *genre_row = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(genre_row);
    lv_obj_set_size(genre_row, 300, 30);
    lv_obj_align(genre_row, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_flex_flow(genre_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(genre_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(genre_row, 8, 0);
    lv_obj_clear_flag(genre_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Genre < */
    lv_obj_t *gp = lv_btn_create(genre_row);
    lv_obj_set_size(gp, 36, 26);
    lv_obj_set_style_bg_color(gp, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(gp, 6, 0);
    lv_obj_add_event_cb(gp, genre_prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gpl = lv_label_create(gp);
    lv_label_set_text(gpl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(gpl, lv_color_hex(th_text), 0);
    lv_obj_center(gpl);

    /* Genre label */
    s_genre_lbl = lv_label_create(genre_row);
    lv_label_set_text_fmt(s_genre_lbl, LV_SYMBOL_AUDIO " %s",
                          genres[s_genre_idx].label);
    lv_obj_set_style_text_font(s_genre_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_genre_lbl, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_width(s_genre_lbl, 180);
    lv_obj_set_style_text_align(s_genre_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* Genre > */
    lv_obj_t *gn = lv_btn_create(genre_row);
    lv_obj_set_size(gn, 36, 26);
    lv_obj_set_style_bg_color(gn, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(gn, 6, 0);
    lv_obj_add_event_cb(gn, genre_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gnl = lv_label_create(gn);
    lv_label_set_text(gnl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(gnl, lv_color_hex(th_text), 0);
    lv_obj_center(gnl);

    /* ── Status label (top-right) ──── */
    s_status_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_status_lbl, "Stopped");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_RIGHT, -10, 10);

    /* ── Station / now-playing label (centre) ──── */
    s_song_lbl = lv_label_create(s_overlay);
    lv_label_set_long_mode(s_song_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_song_lbl, 560);
    lv_label_set_text(s_song_lbl, "Tap " LV_SYMBOL_PLAY " to start");
    lv_obj_set_style_text_font(s_song_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_song_lbl, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_align(s_song_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_song_lbl, LV_ALIGN_CENTER, 0, -10);

    /* ── Transport buttons row ──── */
    lv_obj_t *btn_row = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 320, 44);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 20, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Prev station */
    lv_obj_t *prev_btn = lv_btn_create(btn_row);
    lv_obj_set_size(prev_btn, 80, 38);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(prev_btn, 10, 0);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev_btn);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(th_text), 0);
    lv_obj_center(pl);

    /* Play/Pause */
    lv_obj_t *play_btn = lv_btn_create(btn_row);
    lv_obj_set_size(play_btn, 80, 38);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x2E8B57), 0);
    lv_obj_set_style_radius(play_btn, 10, 0);
    lv_obj_add_event_cb(play_btn, play_pause_cb, LV_EVENT_CLICKED, NULL);
    s_play_btn_lbl = lv_label_create(play_btn);
    lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(s_play_btn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_play_btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_play_btn_lbl);

    /* Next station */
    lv_obj_t *next_btn = lv_btn_create(btn_row);
    lv_obj_set_size(next_btn, 80, 38);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(next_btn, 10, 0);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next_btn);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(th_text), 0);
    lv_obj_center(nl);

    /* ── Vertical volume control (right edge) ──── */
    lv_obj_t *vol_col = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(vol_col);
    lv_obj_set_size(vol_col, 36, 152);
    lv_obj_align(vol_col, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_flex_flow(vol_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vol_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(vol_col, 4, 0);
    lv_obj_clear_flag(vol_col, LV_OBJ_FLAG_SCROLLABLE);

    /* Vol + button (top) */
    lv_obj_t *vup = lv_btn_create(vol_col);
    lv_obj_set_size(vup, 30, 26);
    lv_obj_set_style_bg_color(vup, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(vup, 6, 0);
    lv_obj_add_event_cb(vup, vol_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vul = lv_label_create(vup);
    lv_label_set_text(vul, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(vul, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(vul, &lv_font_montserrat_14, 0);
    lv_obj_center(vul);

    /* Vertical volume slider */
    lv_obj_t *vol_slider = lv_slider_create(vol_col);
    lv_obj_set_size(vol_slider, 10, 80);
    lv_slider_set_range(vol_slider, 0, 255);
    lv_slider_set_value(vol_slider, g_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(th_btn), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x2ECC71), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x2ECC71), LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol_slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Vol – button (bottom) */
    lv_obj_t *vdn = lv_btn_create(vol_col);
    lv_obj_set_size(vdn, 30, 26);
    lv_obj_set_style_bg_color(vdn, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(vdn, 6, 0);
    lv_obj_add_event_cb(vdn, vol_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vdl = lv_label_create(vdn);
    lv_label_set_text(vdl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(vdl, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(vdl, &lv_font_montserrat_14, 0);
    lv_obj_center(vdl);

    /* Volume percentage label */
    s_vol_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_vol_lbl, LV_SYMBOL_VOLUME_MAX " %d%%",
                          g_volume * 100 / 255);
    lv_obj_set_style_text_font(s_vol_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(th_label), 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* ── Station index indicator (bottom-left) ──── */
    s_idx_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_idx_lbl, "%s  %d/%d",
                          genres[s_genre_idx].label,
                          s_genre_idx + 1, (int)GENRE_COUNT);
    lv_obj_set_style_text_font(s_idx_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_idx_lbl, lv_color_hex(th_label), 0);
    lv_obj_align(s_idx_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -4);
}

/* ================================================================
 *  Public API
 * ================================================================ */
void music_player_open(lv_obj_t *parent)
{
    if (s_active) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_active          = true;
    s_close_requested = false;
    s_genre_switch    = -1;
    s_play_requested  = false;
    s_pause_toggle    = false;
    s_skip_next       = false;
    s_skip_prev       = false;
    s_state           = PS_IDLE;

    build_ui();
    ESP_LOGI(TAG, "Radio player opened");
}

void music_player_close(void)
{
    if (!s_active) return;

    s_stop_requested = true;

    int wait = 0;
    while ((s_stream_task || s_decode_task) && wait < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
    }

    if (s_mp3_buf) {
        vStreamBufferDelete(s_mp3_buf);
        s_mp3_buf = NULL;
    }
    if (s_mp3_buf_storage) {
        heap_caps_free(s_mp3_buf_storage);
        s_mp3_buf_storage = NULL;
    }

    s_song_lbl     = NULL;
    s_status_lbl   = NULL;
    s_play_btn_lbl = NULL;
    s_idx_lbl      = NULL;
    s_genre_lbl    = NULL;
    s_vol_lbl      = NULL;

    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_state  = PS_IDLE;
    ESP_LOGI(TAG, "Radio player closed");
}

bool music_player_is_active(void)
{
    return s_active;
}

void music_player_set_volume(uint8_t vol)
{
    if (s_codec_dev) {
        esp_codec_dev_set_out_vol(s_codec_dev, vol * 100 / 255);
    }
}

/* ================================================================
 *  Periodic update  (called from screen_menu_update, ~100 ms)
 * ================================================================ */
void music_player_update(void)
{
    if (!s_active) return;

    /* ── Handle close request ─────────────────────────────── */
    if (s_close_requested) {
        s_stop_requested = true;
        if (s_stream_task == NULL && s_decode_task == NULL) {
            if (s_mp3_buf) {
                vStreamBufferDelete(s_mp3_buf);
                s_mp3_buf = NULL;
            }
            if (s_mp3_buf_storage) {
                heap_caps_free(s_mp3_buf_storage);
                s_mp3_buf_storage = NULL;
            }
            s_song_lbl     = NULL;
            s_status_lbl   = NULL;
            s_play_btn_lbl = NULL;
            s_idx_lbl      = NULL;
            s_genre_lbl    = NULL;
            s_vol_lbl      = NULL;
            if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
            s_active          = false;
            s_close_requested = false;
            s_state           = PS_IDLE;
        }
        return;
    }

    /* ── Handle genre switch (stop current, switch, restart) ── */
    if (s_genre_switch >= 0) {
        if (s_state == PS_IDLE || s_state == PS_ERROR) {
            /* Not playing — just switch genre label */
            s_genre_idx    = s_genre_switch;
            s_genre_switch = -1;
            if (s_genre_lbl)
                lv_label_set_text_fmt(s_genre_lbl, LV_SYMBOL_AUDIO " %s",
                                      genres[s_genre_idx].label);
            if (s_idx_lbl)
                lv_label_set_text_fmt(s_idx_lbl, "%s  %d/%d",
                                      genres[s_genre_idx].label,
                                      s_genre_idx + 1, (int)GENRE_COUNT);
            if (s_song_lbl)
                lv_label_set_text(s_song_lbl, "Tap " LV_SYMBOL_PLAY " to start");
        } else {
            /* Playing — stop first, then switch on next cycle */
            s_stop_requested = true;
            if (s_stream_task == NULL && s_decode_task == NULL) {
                s_genre_idx      = s_genre_switch;
                s_genre_switch   = -1;
                s_state          = PS_IDLE;
                if (s_genre_lbl)
                    lv_label_set_text_fmt(s_genre_lbl, LV_SYMBOL_AUDIO " %s",
                                          genres[s_genre_idx].label);
                /* Auto-start the new genre */
                s_play_requested = true;
            }
        }
        return;
    }

    /* ── Handle play request ──────────────────────────────── */
    if (s_play_requested) {
        s_play_requested = false;
        if (s_state == PS_IDLE || s_state == PS_ERROR) {
            do_start_playback();
            if (s_play_btn_lbl)
                lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PAUSE);
        }
    }

    /* ── Handle pause toggle ──────────────────────────────── */
    if (s_pause_toggle) {
        s_pause_toggle = false;
        if (s_state == PS_PLAYING) {
            s_state = PS_PAUSED;
            s_state_changed = true;
            if (s_play_btn_lbl)
                lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PLAY);
        } else if (s_state == PS_PAUSED) {
            s_state = PS_PLAYING;
            s_state_changed = true;
            if (s_play_btn_lbl)
                lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PAUSE);
        }
    }

    /* ── Update UI labels from background state ───────────── */
    if (s_meta_updated && s_song_lbl) {
        s_meta_updated = false;
        lv_label_set_text(s_song_lbl, s_song_title);
        if (s_idx_lbl && s_station_count > 0)
            lv_label_set_text_fmt(s_idx_lbl, "Song %d/%d  |  %s",
                                  s_current_station + 1, s_station_count,
                                  genres[s_genre_idx].label);
    }

    if (s_state_changed && s_status_lbl) {
        s_state_changed = false;
        const char *txt = "Stopped";
        uint32_t   col  = 0x888888;
        switch (s_state) {
            case PS_CONNECTING: txt = "Connecting..."; col = 0xFFCC00; break;
            case PS_PLAYING:    txt = LV_SYMBOL_AUDIO " Playing"; col = 0x00FF88; break;
            case PS_PAUSED:     txt = "Paused";        col = 0xFFCC00; break;
            case PS_ERROR:      txt = "Error";         col = 0xFF4444; break;
            default: break;
        }
        lv_label_set_text(s_status_lbl, txt);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(col), 0);
    }
}
