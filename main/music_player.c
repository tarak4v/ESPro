/**
 * @file music_player.c
 * @brief Internet radio Bollywood music player with MP3 streaming.
 *
 * Streams from online radio stations, decodes MP3 via minimp3,
 * outputs PCM via I2S → ES8311 codec → PA speaker.
 * Parses ICY metadata for song/artist info.
 *
 * Pipeline:  HTTP stream → ring buffer → MP3 decode → I2S
 */

#include "music_player.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "screen_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/i2s_std.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "minimp3.h"

static const char *TAG = "music";

/* ================================================================
 *  Station list (update URLs as needed)
 * ================================================================ */
typedef struct {
    const char *name;
    const char *url;
} station_t;

static const station_t stations[] = {
    { "Bolly Hits FM",    "http://stream.zeno.fm/fyn8eh3h5tzuv" },
    { "Hindi Gaane",      "http://stream.zeno.fm/hqn0dn4rdk8uv" },
    { "Desi Beats",       "http://stream.zeno.fm/r4b56b4dn78uv" },
    { "Bollywood Lounge", "http://stream.zeno.fm/7n2g4bb2dk8uv" },
    { "Radio Nasha",      "http://stream.zeno.fm/p1sfgffqpgruv" },
};
#define STATION_COUNT  (sizeof(stations) / sizeof(stations[0]))

/* ================================================================
 *  Player state
 * ================================================================ */
typedef enum {
    PS_IDLE, PS_CONNECTING, PS_PLAYING, PS_PAUSED, PS_ERROR
} player_state_t;

static volatile player_state_t s_state = PS_IDLE;
static int  s_station_idx = 0;
static bool s_active      = false;

/* Song info from ICY metadata */
static char s_song_title[128]  = "---";
static char s_station_name[64] = "";
static volatile bool s_meta_updated  = false;
static volatile bool s_state_changed = false;

/* Deferred commands (set by UI callbacks, processed in update) */
static volatile bool s_close_requested   = false;
static volatile int  s_switch_station    = -1;   /* >=0 → pending switch */
static volatile bool s_play_requested    = false;
static volatile bool s_pause_toggle      = false;

/* ================================================================
 *  Audio pipeline handles
 * ================================================================ */
#define MP3_BUF_SIZE        (24 * 1024)
#define MP3_INPUT_BUF_SIZE  (8 * 1024)

static StreamBufferHandle_t s_mp3_buf    = NULL;
static TaskHandle_t         s_stream_task = NULL;
static TaskHandle_t         s_decode_task = NULL;
static volatile bool        s_stop_requested = false;

static i2s_chan_handle_t         s_i2s_tx = NULL;
static esp_io_expander_handle_t  s_io_exp = NULL;
static mp3dec_t                  s_mp3dec;

/* ICY metadata interval (set by HTTP event handler) */
static volatile int s_icy_metaint = 0;

/* ================================================================
 *  LVGL objects
 * ================================================================ */
static lv_obj_t *s_overlay      = NULL;
static lv_obj_t *s_station_lbl  = NULL;
static lv_obj_t *s_song_lbl     = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_play_btn_lbl = NULL;
static lv_obj_t *s_idx_lbl      = NULL;

/* ================================================================
 *  ES8311 codec helpers
 * ================================================================ */
static void es8311_write(uint8_t reg, uint8_t val)
{
    i2c_writr_buff(es8311_dev_handle, reg, &val, 1);
}

static void es8311_codec_init(void)
{
    es8311_write(0x00, 0x1F);  vTaskDelay(pdMS_TO_TICKS(20));
    es8311_write(0x00, 0x00);
    es8311_write(0x01, 0x30);
    es8311_write(0x02, 0x00);
    es8311_write(0x03, 0x10);
    es8311_write(0x04, 0x10);
    es8311_write(0x05, 0x00);
    es8311_write(0x06, 0x03);
    es8311_write(0x07, 0x00);
    es8311_write(0x08, 0xFF);
    es8311_write(0x09, 0x0C);
    es8311_write(0x0A, 0x0C);
    es8311_write(0x0B, 0x00);
    es8311_write(0x0C, 0x00);
    es8311_write(0x10, 0x1F);
    es8311_write(0x11, 0x7F);
    es8311_write(0x00, 0x80);  vTaskDelay(pdMS_TO_TICKS(50));
    es8311_write(0x0D, 0x01);
    es8311_write(0x12, 0x00);
    es8311_write(0x13, 0x10);
    es8311_write(0x14, 0x1A);
    es8311_write(0x32, g_volume);  /* Use settings volume */
}

/* ================================================================
 *  Audio hardware init / deinit  (called from decode task)
 * ================================================================ */
static void audio_init(int sample_rate)
{
    /* Power amplifier via TCA9554 */
    if (!s_io_exp) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(
            i2c_port0_bus_handle,
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
            &s_io_exp);
        if (ret != ESP_OK)
            ESP_LOGW(TAG, "TCA9554: %s", esp_err_to_name(ret));
    }
    if (s_io_exp) {
        esp_io_expander_set_dir(s_io_exp, IO_EXPANDER_PIN_NUM_7,
                                IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(s_io_exp, IO_EXPANDER_PIN_NUM_7, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    es8311_codec_init();

    /* I2S TX channel */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed");
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_NUM_I2S_MCLK,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws   = PIN_NUM_I2S_WS,
            .dout = PIN_NUM_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    i2s_channel_enable(s_i2s_tx);
    ESP_LOGI(TAG, "Audio init: %d Hz mono", sample_rate);
}

static void audio_deinit(void)
{
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_io_exp) {
        esp_io_expander_set_level(s_io_exp, IO_EXPANDER_PIN_NUM_7, 0);
    }
}

/* ================================================================
 *  ICY metadata parser
 * ================================================================ */
static void parse_icy_meta(const char *meta, int len)
{
    const char *key = "StreamTitle='";
    const char *p = strstr(meta, key);
    if (!p) return;
    p += strlen(key);
    const char *end = strchr(p, '\'');
    if (!end || end <= p) return;

    int tlen = end - p;
    if (tlen >= (int)sizeof(s_song_title)) tlen = sizeof(s_song_title) - 1;
    memcpy(s_song_title, p, tlen);
    s_song_title[tlen] = '\0';
    s_meta_updated = true;
    ESP_LOGI(TAG, "Now: %s", s_song_title);
}

/* ================================================================
 *  HTTP event handler — captures ICY headers
 * ================================================================ */
static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_icy_metaint = atoi(evt->header_value);
            ESP_LOGI(TAG, "icy-metaint: %d", s_icy_metaint);
        }
        if (strcasecmp(evt->header_key, "icy-name") == 0) {
            strncpy(s_station_name, evt->header_value,
                    sizeof(s_station_name) - 1);
            s_station_name[sizeof(s_station_name) - 1] = '\0';
            s_meta_updated = true;
        }
    }
    return ESP_OK;
}

/* ================================================================
 *  HTTP streaming task
 * ================================================================ */
static void stream_task(void *arg)
{
    const station_t *stn = &stations[s_station_idx];
    ESP_LOGI(TAG, "Stream: %s → %s", stn->name, stn->url);

    strncpy(s_station_name, stn->name, sizeof(s_station_name) - 1);
    s_station_name[sizeof(s_station_name) - 1] = '\0';
    strncpy(s_song_title, "Connecting...", sizeof(s_song_title) - 1);
    s_meta_updated  = true;
    s_state         = PS_CONNECTING;
    s_state_changed = true;
    s_icy_metaint   = 0;

    esp_http_client_config_t cfg = {
        .url          = stn->url,
        .timeout_ms   = 15000,
        .buffer_size  = 4096,
        .event_handler = http_evt_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_http_client_set_header(client, "Icy-MetaData", "1");
    esp_http_client_set_header(client, "User-Agent", "ESP32-Radio/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Open failed: %s", esp_err_to_name(err));
        strncpy(s_song_title, "Connection failed", sizeof(s_song_title) - 1);
        s_meta_updated  = true;
        s_state         = PS_ERROR;
        s_state_changed = true;
        esp_http_client_cleanup(client);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_fetch_headers(client);

    s_state         = PS_PLAYING;
    s_state_changed = true;
    strncpy(s_song_title, stn->name, sizeof(s_song_title) - 1);
    s_meta_updated = true;

    uint8_t buf[2048];
    int meta_counter = 0;  /* bytes since last ICY metadata block */

    while (!s_stop_requested) {
        int to_read = (int)sizeof(buf);

        /* If ICY metadata is interleaved, limit read to next meta block */
        if (s_icy_metaint > 0) {
            int left = s_icy_metaint - meta_counter;
            if (left < to_read) to_read = left;
            if (to_read <= 0) to_read = 1;
        }

        int n = esp_http_client_read(client, (char *)buf, to_read);
        if (n <= 0) {
            ESP_LOGW(TAG, "Stream %s (n=%d)",
                     n == 0 ? "ended" : "error", n);
            break;
        }

        /* Push audio data to ring buffer */
        xStreamBufferSend(s_mp3_buf, buf, n, pdMS_TO_TICKS(500));
        meta_counter += n;

        /* Parse ICY metadata block */
        if (s_icy_metaint > 0 && meta_counter >= s_icy_metaint) {
            uint8_t mlen_byte;
            int r = esp_http_client_read(client, (char *)&mlen_byte, 1);
            if (r == 1 && mlen_byte > 0) {
                int mlen = mlen_byte * 16;
                char meta[4096];
                int got = 0;
                while (got < mlen && !s_stop_requested) {
                    r = esp_http_client_read(client, meta + got, mlen - got);
                    if (r <= 0) break;
                    got += r;
                }
                if (got > 0) {
                    meta[got] = '\0';
                    parse_icy_meta(meta, got);
                }
            }
            meta_counter = 0;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_state == PS_PLAYING || s_state == PS_CONNECTING) {
        strncpy(s_song_title, "Stream ended", sizeof(s_song_title) - 1);
        s_meta_updated  = true;
        s_state         = PS_ERROR;
        s_state_changed = true;
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
    mp3dec_init(&s_mp3dec);

    bool hw_ok = false;
    int fill   = 0;

    /* Allocate large buffers from PSRAM */
    uint8_t *mp3_in = heap_caps_malloc(MP3_INPUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    int16_t *pcm    = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME *
                                       sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *mono   = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME *
                                       sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!mp3_in || !pcm || !mono) {
        ESP_LOGE(TAG, "Decode buf alloc failed");
        goto done;
    }
    mp3dec_frame_info_t info;

    ESP_LOGI(TAG, "Decode task started");

    while (!s_stop_requested) {
        /* Pause — keep task alive but don't decode */
        if (s_state == PS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Fill input buffer from stream */
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

        /* Decode one MP3 frame */
        int samples = mp3dec_decode_frame(&s_mp3dec, mp3_in, fill,
                                          pcm, &info);

        if (info.frame_bytes > 0) {
            fill -= info.frame_bytes;
            if (fill > 0)
                memmove(mp3_in, mp3_in + info.frame_bytes, fill);
        }

        if (samples > 0) {
            /* First good frame — init audio hardware */
            if (!hw_ok) {
                audio_init(info.hz > 0 ? info.hz : 44100);
                hw_ok = true;
                ESP_LOGI(TAG, "Audio: %dHz %dch", info.hz, info.channels);
            }

            /* Downmix stereo to mono for ES8311 */
            int16_t *out = pcm;
            int out_samples = samples;
            if (info.channels == 2) {
                for (int i = 0; i < samples; i++)
                    mono[i] = (int16_t)(((int32_t)pcm[2*i] + pcm[2*i+1]) / 2);
                out = mono;
            }

            /* Write to I2S */
            size_t written;
            i2s_channel_write(s_i2s_tx, out,
                              out_samples * sizeof(int16_t),
                              &written, pdMS_TO_TICKS(500));
        } else if (info.frame_bytes == 0 && fill > 0) {
            /* Sync lost — skip ahead to next sync word (0xFF 0xEx) */
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
 *  Start / stop helpers  (non-blocking, safe from any context)
 * ================================================================ */
static void do_start_playback(void)
{
    s_stop_requested = false;

    if (!s_mp3_buf)
        s_mp3_buf = xStreamBufferCreate(MP3_BUF_SIZE, 1);
    else
        xStreamBufferReset(s_mp3_buf);

    xTaskCreatePinnedToCore(stream_task, "radio_rx",  16384,
                            NULL, 3, &s_stream_task, 1);
    xTaskCreatePinnedToCore(decode_task, "mp3_dec",   16384,
                            NULL, 4, &s_decode_task, 1);
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
    s_switch_station = (s_station_idx + 1) % STATION_COUNT;
    s_stop_requested = true;
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    s_switch_station = (s_station_idx + STATION_COUNT - 1) % STATION_COUNT;
    s_stop_requested = true;
}

/* ================================================================
 *  Build overlay UI  (640×172)
 * ================================================================ */
static void build_ui(void)
{
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);

    /* ── Back button (top-left) ──── */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* ── Station label (top-centre) ──── */
    s_station_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_station_lbl, LV_SYMBOL_AUDIO " %s",
                          stations[s_station_idx].name);
    lv_obj_set_style_text_font(s_station_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_station_lbl, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(s_station_lbl, LV_ALIGN_TOP_MID, 0, 8);

    /* ── Status label (top-right) ──── */
    s_status_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_status_lbl, "Stopped");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_RIGHT, -10, 10);

    /* ── Song / now-playing label (centre) ──── */
    s_song_lbl = lv_label_create(s_overlay);
    lv_label_set_long_mode(s_song_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_song_lbl, 560);
    lv_label_set_text(s_song_lbl, "Tap " LV_SYMBOL_PLAY " to start");
    lv_obj_set_style_text_font(s_song_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_song_lbl, lv_color_hex(0xFFFFFF), 0);
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

    /* Prev */
    lv_obj_t *prev_btn = lv_btn_create(btn_row);
    lv_obj_set_size(prev_btn, 80, 38);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(prev_btn, 10, 0);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev_btn);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pl);

    /* Play/Pause */
    lv_obj_t *play_btn = lv_btn_create(btn_row);
    lv_obj_set_size(play_btn, 80, 38);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x7B6800), 0);
    lv_obj_set_style_radius(play_btn, 10, 0);
    lv_obj_add_event_cb(play_btn, play_pause_cb, LV_EVENT_CLICKED, NULL);
    s_play_btn_lbl = lv_label_create(play_btn);
    lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(s_play_btn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_play_btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_play_btn_lbl);

    /* Next */
    lv_obj_t *next_btn = lv_btn_create(btn_row);
    lv_obj_set_size(next_btn, 80, 38);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(next_btn, 10, 0);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next_btn);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(nl);

    /* ── Station index indicator (bottom) ──── */
    s_idx_lbl = lv_label_create(s_overlay);
    lv_label_set_text_fmt(s_idx_lbl, "%d / %d",
                          s_station_idx + 1, (int)STATION_COUNT);
    lv_obj_set_style_text_font(s_idx_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_idx_lbl, lv_color_hex(0x666666), 0);
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
    s_switch_station  = -1;
    s_play_requested  = false;
    s_pause_toggle    = false;
    s_state           = PS_IDLE;

    build_ui();
    ESP_LOGI(TAG, "Music player opened");
}

void music_player_close(void)
{
    if (!s_active) return;

    /* Signal tasks to stop */
    s_stop_requested = true;

    /* Wait briefly (from non-LVGL context only; from UI it'll be deferred) */
    int wait = 0;
    while ((s_stream_task || s_decode_task) && wait < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
    }

    if (s_mp3_buf) {
        vStreamBufferDelete(s_mp3_buf);
        s_mp3_buf = NULL;
    }

    s_station_lbl  = NULL;
    s_song_lbl     = NULL;
    s_status_lbl   = NULL;
    s_play_btn_lbl = NULL;
    s_idx_lbl      = NULL;

    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_active = false;
    s_state  = PS_IDLE;
    ESP_LOGI(TAG, "Music player closed");
}

bool music_player_is_active(void)
{
    return s_active;
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
            s_station_lbl  = NULL;
            s_song_lbl     = NULL;
            s_status_lbl   = NULL;
            s_play_btn_lbl = NULL;
            s_idx_lbl      = NULL;
            if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
            s_active          = false;
            s_close_requested = false;
            s_state           = PS_IDLE;
        }
        return;
    }

    /* ── Handle station switch ────────────────────────────── */
    if (s_switch_station >= 0) {
        if (s_stream_task == NULL && s_decode_task == NULL) {
            s_station_idx   = s_switch_station;
            s_switch_station = -1;
            do_start_playback();
            if (s_play_btn_lbl)
                lv_label_set_text(s_play_btn_lbl, LV_SYMBOL_PAUSE);
            if (s_idx_lbl)
                lv_label_set_text_fmt(s_idx_lbl, "%d / %d",
                                      s_station_idx + 1, (int)STATION_COUNT);
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
        if (s_station_lbl)
            lv_label_set_text_fmt(s_station_lbl, LV_SYMBOL_AUDIO " %s",
                                  s_station_name);
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
