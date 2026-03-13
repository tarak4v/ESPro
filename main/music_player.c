/**
 * @file music_player.c
 * @brief Music player using JioSaavn search API + DES-ECB URL decryption.
 *
 * Pipeline:  JioSaavn API → decrypt URL → HTTPS MP3 stream → minimp3 → I2S
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
 *  Song / genre definitions (JioSaavn)
 * ================================================================ */
#define MAX_STATIONS    10
#define NAME_LEN        128
#define URL_LEN         300

typedef struct {
    char name[NAME_LEN];
    char url[URL_LEN];
} station_t;

typedef struct {
    const char *label;   /* display name */
    const char *query;   /* JioSaavn search query */
} genre_t;

static const genre_t genres[] = {
    { "BW Latest",  "new bollywood songs 2024" },
    { "BW 90s",     "90s bollywood hits" },
    { "Bhajans",    "popular bhajans" },
    { "Dance Mix",  "bollywood dance party" },
    { "Lofi",       "lofi chill hindi" },
};
#define GENRE_COUNT (sizeof(genres)/sizeof(genres[0]))

/* ── Dynamic song list (fetched from JioSaavn) ── */
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

/* ================================================================
 *  Audio pipeline handles
 * ================================================================ */
#define MP3_BUF_SIZE        (96 * 1024)
#define MP3_INPUT_BUF_SIZE  (8 * 1024)
#define PRE_BUFFER_BYTES    (24 * 1024)
#define API_BUF_SIZE        (48 * 1024)

static StreamBufferHandle_t s_mp3_buf    = NULL;
static TaskHandle_t         s_stream_task = NULL;
static TaskHandle_t         s_decode_task = NULL;
static volatile bool        s_stop_requested = false;

static i2s_chan_handle_t         s_i2s_tx = NULL;
static mp3dec_t                  s_mp3dec;

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
        .channel         = 2,
        .sample_rate     = (uint32_t)sample_rate,
        .mclk_multiple   = 256,
    };
    esp_codec_dev_open(s_codec_dev, &fs);
    esp_codec_dev_set_out_vol(s_codec_dev, (float)g_volume * 127.5f / 255.0f - 95.5f);

    ESP_LOGI(TAG, "Audio init: %d Hz stereo, vol=%d", sample_rate, g_volume);
}

static void audio_deinit(void)
{
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    /* TX channel is shared — don't delete, just clear our pointer */
    s_i2s_tx = NULL;
    s_codec_if = NULL;
    s_ctrl_if  = NULL;
    s_data_if  = NULL;
    s_gpio_if  = NULL;
}

/* ================================================================
 *  JioSaavn DES-ECB URL decryption
 *  Key: "38346591", Base64 decode → DES-ECB decrypt → PKCS#5 unpad
 *  Then rewrite quality to _320.mp3 for best clarity
 * ================================================================ */
static int jiosaavn_decrypt_url(const char *enc, char *out, int out_sz)
{
    static const char des_key[8] = "38346591";

    /* Base64 decode */
    size_t b64_len = 0;
    if (mbedtls_base64_decode(NULL, 0, &b64_len,
                              (const unsigned char *)enc, strlen(enc)) !=
            MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || b64_len == 0)
        return -1;

    uint8_t *cipher = heap_caps_malloc(b64_len + 16, MALLOC_CAP_SPIRAM);
    if (!cipher) return -1;

    size_t olen = 0;
    if (mbedtls_base64_decode(cipher, b64_len + 16, &olen,
                              (const unsigned char *)enc, strlen(enc)) != 0) {
        heap_caps_free(cipher);
        return -1;
    }

    /* DES-ECB decrypt */
    mbedtls_des_context ctx;
    mbedtls_des_init(&ctx);
    mbedtls_des_setkey_dec(&ctx, (const unsigned char *)des_key);

    uint8_t *plain = heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM);
    if (!plain) {
        mbedtls_des_free(&ctx);
        heap_caps_free(cipher);
        return -1;
    }

    for (size_t i = 0; i + 8 <= olen; i += 8)
        mbedtls_des_crypt_ecb(&ctx, cipher + i, plain + i);
    mbedtls_des_free(&ctx);
    heap_caps_free(cipher);

    /* PKCS#5 unpad */
    if (olen == 0) { heap_caps_free(plain); return -1; }
    uint8_t pad = plain[olen - 1];
    if (pad > 0 && pad <= 8 && pad <= olen)
        olen -= pad;
    plain[olen] = '\0';

    /* Rewrite quality: replace _96.mp4 / _320.mp4 etc → _320.mp3
     * 320 kbps MP3 for best clarity that minimp3 can decode */
    char *url_str = (char *)plain;

    /* Find the last underscore before .mp4 or .mp3 */
    char *dot_ext = strstr(url_str, ".mp4");
    if (!dot_ext) dot_ext = strstr(url_str, ".mp3");
    if (dot_ext) {
        /* Walk backward to find the underscore before the bitrate */
        char *us = dot_ext - 1;
        while (us > url_str && *us != '_') us--;
        if (*us == '_') {
            /* Replace _XXX.mp4/.mp3 with _320.mp3 */
            int prefix_len = (int)(us - url_str);
            if (prefix_len + 9 < out_sz) {   /* _320.mp3\0 = 9 */
                memcpy(out, url_str, prefix_len);
                memcpy(out + prefix_len, "_320.mp3", 8);
                out[prefix_len + 8] = '\0';
                heap_caps_free(plain);
                return 0;
            }
        }
    }

    /* Fallback: just copy as-is */
    int len = (int)olen;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, plain, len);
    out[len] = '\0';
    heap_caps_free(plain);
    return 0;
}

/* ================================================================
 *  Fetch songs from JioSaavn search API
 * ================================================================ */
static int fetch_jiosaavn_songs(int genre)
{
    const genre_t *g = &genres[genre];
    ESP_LOGI(TAG, "JioSaavn: searching '%s'", g->query);

    char api_url[400];
    snprintf(api_url, sizeof(api_url),
             "https://www.jiosaavn.com/api.php?"
             "__call=search.getResults&p=1&q=%s"
             "&_format=json&_marker=0&n=%d",
             g->query, MAX_STATIONS);
    /* URL-encode spaces → + */
    for (char *p = api_url; *p; p++)
        if (*p == ' ') *p = '+';

    char *buf = heap_caps_malloc(API_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "API buf alloc failed"); return -1; }

    esp_http_client_config_t cfg = {
        .url              = api_url,
        .timeout_ms       = 15000,
        .buffer_size      = 4096,
        .buffer_size_tx   = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "ESPro/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "API open failed: %s", esp_err_to_name(err));
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

    /* Parse JSON — JioSaavn returns { "results": [ ... ] } */
    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return -1;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results)) {
        ESP_LOGE(TAG, "No 'results' array in response");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(results);
    if (count > MAX_STATIONS) count = MAX_STATIONS;

    int loaded = 0;
    for (int i = 0; i < count && loaded < MAX_STATIONS; i++) {
        cJSON *item = cJSON_GetArrayItem(results, i);
        if (!item) continue;

        cJSON *j_song    = cJSON_GetObjectItem(item, "song");
        cJSON *j_singers = cJSON_GetObjectItem(item, "singers");
        cJSON *j_enc_url = cJSON_GetObjectItem(item, "encrypted_media_url");

        if (!j_enc_url || !j_enc_url->valuestring) continue;

        /* Build display name: "Song - Artist" */
        const char *song_name = (j_song && j_song->valuestring)
                                    ? j_song->valuestring : "Unknown";
        const char *singers   = (j_singers && j_singers->valuestring)
                                    ? j_singers->valuestring : "";
        snprintf(s_stations[loaded].name, NAME_LEN, "%s - %s",
                 song_name, singers);
        s_stations[loaded].name[NAME_LEN - 1] = '\0';

        /* Decrypt URL and rewrite to MP3 */
        if (jiosaavn_decrypt_url(j_enc_url->valuestring,
                                  s_stations[loaded].url, URL_LEN) != 0) {
            ESP_LOGW(TAG, "Decrypt failed for song %d", i);
            continue;
        }

        ESP_LOGI(TAG, "Song %d: %.60s → %.80s",
                 loaded, s_stations[loaded].name, s_stations[loaded].url);
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
        /* Fisher-Yates shuffle for variety */
        for (int i = s_station_count - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            station_t tmp = s_stations[i];
            s_stations[i] = s_stations[j];
            s_stations[j] = tmp;
        }
    }
}

/* ================================================================
 *  Stream a song (HTTP/HTTPS MP3 download → ring buffer)
 * ================================================================ */
static void stream_radio(const char *url)
{
    ESP_LOGI(TAG, "Radio: connecting to %.120s", url);

    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 15000,
        .buffer_size    = 8192,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "ESPro/1.0");

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

    ESP_LOGI(TAG, "Stream connected (HTTP %d), pumping data", status);

    s_state = PS_PLAYING;
    s_state_changed = true;

    uint8_t buf[4096];
    int total_bytes = 0;
    bool pre_buf_logged = false;

    while (!s_stop_requested) {
        if (s_state == PS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_skip_next || s_skip_prev) break;

        int n = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (n <= 0) {
            ESP_LOGI(TAG, "Song ended (n=%d, total=%d bytes)", n, total_bytes);
            break;
        }
        total_bytes += n;
        if (total_bytes == n)
            ESP_LOGI(TAG, "First data chunk: %d bytes, header: %02X %02X %02X %02X",
                     n, buf[0], buf[1], buf[2], buf[3]);
        if (!pre_buf_logged && total_bytes >= PRE_BUFFER_BYTES) {
            ESP_LOGI(TAG, "Pre-buffer ready, starting decode");
            pre_buf_logged = true;
        }
        xStreamBufferSend(s_mp3_buf, buf, n, pdMS_TO_TICKS(500));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* ================================================================
 *  Streaming task — loads songs, then streams them one by one
 * ================================================================ */
static void stream_task(void *arg)
{
    /* Load stations here (not in do_start_playback) because
     * JioSaavn API uses HTTPS/TLS which needs ~16KB stack */
    load_stations(s_genre_idx);
    if (s_station_count == 0) {
        snprintf(s_song_title, sizeof(s_song_title), "No songs found");
        s_meta_updated  = true;
        s_state         = PS_ERROR;
        s_state_changed = true;
        ESP_LOGW(TAG, "No songs fetched, stream task exit");
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!s_stop_requested) {
        if (s_current_station >= s_station_count) {
            /* Wrap around to loop the playlist */
            s_current_station = 0;
        }

        station_t *st = &s_stations[s_current_station];
        snprintf(s_station_name, sizeof(s_station_name), "%s",
                 genres[s_genre_idx].label);
        snprintf(s_song_title, sizeof(s_song_title),
                 "Song %d/%d\n%s",
                 s_current_station + 1, s_station_count, st->name);
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
            continue;
        }
        if (s_skip_prev) {
            s_skip_prev = false;
            s_current_station = (s_current_station + s_station_count - 1) % s_station_count;
            xStreamBufferReset(s_mp3_buf);
            continue;
        }

        /* Song ended normally — auto-advance to next */
        ESP_LOGI(TAG, "Song ended, advancing to next");
        s_current_station = (s_current_station + 1) % s_station_count;
        xStreamBufferReset(s_mp3_buf);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Stream task exit");
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  MP3 decode + I2S output task
 * ================================================================ */
static void decode_task(void *arg)
{
    ESP_LOGI(TAG, "Decode task entered (stack free: %d)",
             uxTaskGetStackHighWaterMark(NULL));

    bool hw_ok = false;
    int fill   = 0;
    uint8_t *mp3_in = NULL;
    int16_t *pcm    = NULL;
    int16_t *mono   = NULL;

    if (!s_mp3_buf) {
        ESP_LOGE(TAG, "Decode: stream buffer is NULL!");
        goto done;
    }

    mp3dec_init(&s_mp3dec);

    mp3_in = heap_caps_malloc(MP3_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    pcm    = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME *
                                       sizeof(int16_t), MALLOC_CAP_SPIRAM);
    mono   = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME *
                                       2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!mp3_in || !pcm || !mono) {
        ESP_LOGE(TAG, "Decode buf alloc failed");
        goto done;
    }
    mp3dec_frame_info_t info;

    ESP_LOGI(TAG, "Decode task started");

    int total_decoded = 0;
    int skip_count    = 0;

    while (!s_stop_requested) {
        if (s_state == PS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int space = MP3_INPUT_BUF_SIZE - fill;
        if (space > 0) {
            size_t got = xStreamBufferReceive(s_mp3_buf,
                            mp3_in + fill, space, pdMS_TO_TICKS(100));
            fill += (int)got;
        }
        if (fill < 512) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int samples = mp3dec_decode_frame(&s_mp3dec, mp3_in, fill,
                                          pcm, &info);

        if (info.frame_bytes > 0) {
            fill -= info.frame_bytes;
            if (fill > 0)
                memmove(mp3_in, mp3_in + info.frame_bytes, fill);
        }

        if (samples > 0) {
            if (!hw_ok) {
                ESP_LOGI(TAG, "First MP3 frame: %dHz %dch %dkbps, calling audio_init",
                         info.hz, info.channels, info.bitrate_kbps);
                audio_init(info.hz > 0 ? info.hz : 44100);
                hw_ok = true;
                ESP_LOGI(TAG, "Audio: %dHz %dch %dkbps", info.hz, info.channels,
                         info.bitrate_kbps);
            }
            total_decoded++;

            int16_t *out;
            int out_bytes;

            if (info.channels == 2) {
                /* Stereo: output interleaved L R pairs directly */
                out = pcm;
                out_bytes = samples * 2 * sizeof(int16_t);
            } else {
                /* Mono: duplicate to stereo L R for proper I2S framing */
                for (int i = samples - 1; i >= 0; i--) {
                    mono[2*i]     = pcm[i];
                    mono[2*i + 1] = pcm[i];
                }
                out = mono;
                out_bytes = samples * 2 * sizeof(int16_t);
            }

            /* Pre-emphasis: boost treble for clarity on small speaker.
             * y[n] = x[n] + coeff * (x[n] - x[n-1])
             * coeff = 48/64 ≈ 0.75 → ~6 dB/oct above ~2 kHz */
            {
                static int16_t prev_l = 0, prev_r = 0;
                int n_stereo = out_bytes / (2 * sizeof(int16_t));
                for (int i = 0; i < n_stereo; i++) {
                    int32_t l = out[2*i];
                    int32_t r = out[2*i + 1];
                    int32_t el = l + ((l - prev_l) * 48 >> 6);
                    int32_t er = r + ((r - prev_r) * 48 >> 6);
                    prev_l = (int16_t)l;
                    prev_r = (int16_t)r;
                    /* Clamp to int16 range */
                    if (el > 32767) el = 32767; else if (el < -32768) el = -32768;
                    if (er > 32767) er = 32767; else if (er < -32768) er = -32768;
                    out[2*i]     = (int16_t)el;
                    out[2*i + 1] = (int16_t)er;
                }
            }

            size_t written;
            i2s_channel_write(s_i2s_tx, out, out_bytes,
                              &written, pdMS_TO_TICKS(500));
        } else if (info.frame_bytes == 0 && fill > 0) {
            skip_count++;
            if (skip_count <= 5 || (skip_count % 100) == 0)
                ESP_LOGW(TAG, "No sync, skipping byte (skip=%d, fill=%d)",
                         skip_count, fill);
            int skip = 1;
            for (int i = 1; i < fill - 1; i++) {
                if (mp3_in[i] == 0xFF && (mp3_in[i+1] & 0xE0) == 0xE0) {
                    skip = i;
                    break;
                }
            }
            fill -= skip;
            if (fill > 0)
                memmove(mp3_in, mp3_in + skip, fill);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

done:
    if (hw_ok) audio_deinit();
    heap_caps_free(mp3_in);
    heap_caps_free(pcm);
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

    /* Show loading message — load_stations() runs in stream_task */
    snprintf(s_song_title, sizeof(s_song_title), "Loading %s...",
             genres[s_genre_idx].label);
    s_meta_updated = true;

    if (!s_mp3_buf) {
        s_mp3_buf = xStreamBufferCreate(MP3_BUF_SIZE, 1);
        if (!s_mp3_buf) {
            ESP_LOGE(TAG, "Stream buffer alloc failed (%d bytes)!", MP3_BUF_SIZE);
            s_state = PS_ERROR;
            s_state_changed = true;
            return;
        }
    } else {
        xStreamBufferReset(s_mp3_buf);
    }

    /* stream_task needs 24KB for HTTPS/TLS (JioSaavn API + CDN) */
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

    /* ── Station index indicator (bottom) ──── */
    s_idx_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_idx_lbl, "%s  (%d/%d)",
                          genres[s_genre_idx].label,
                          s_genre_idx + 1, (int)GENRE_COUNT);
    lv_obj_set_style_text_font(s_idx_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_idx_lbl, lv_color_hex(th_label), 0);
    lv_obj_align(s_idx_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
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
    ESP_LOGI(TAG, "Music player opened");
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

    s_song_lbl     = NULL;
    s_status_lbl   = NULL;
    s_play_btn_lbl = NULL;
    s_idx_lbl      = NULL;
    s_genre_lbl    = NULL;

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
        float db = (float)vol * 127.5f / 255.0f - 95.5f;
        esp_codec_dev_set_out_vol(s_codec_dev, db);
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
            s_song_lbl     = NULL;
            s_status_lbl   = NULL;
            s_play_btn_lbl = NULL;
            s_idx_lbl      = NULL;
            s_genre_lbl    = NULL;
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
            lv_label_set_text_fmt(s_idx_lbl, "Station %d/%d  |  %s",
                                  s_current_station + 1, s_station_count,
                                  genres[s_genre_idx].label);
    }

    if (s_state_changed && s_status_lbl) {
        s_state_changed = false;
        const char *txt = "Stopped";
        uint32_t   col  = 0x888888;
        switch (s_state) {
            case PS_CONNECTING: txt = "Connecting..."; col = 0xFFCC00; break;
            case PS_PLAYING:    txt = LV_SYMBOL_AUDIO " Live";  col = 0x00FF88; break;
            case PS_PAUSED:     txt = "Paused";        col = 0xFFCC00; break;
            case PS_ERROR:      txt = "Error";         col = 0xFF4444; break;
            default: break;
        }
        lv_label_set_text(s_status_lbl, txt);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(col), 0);
    }
}
