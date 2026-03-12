/**
 * @file boot_sound.c
 * @brief Nokia-style startup melody + reusable melody player.
 *
 * Provides:
 *   boot_sound_play()     — play Nokia tune (blocking)
 *   play_melody()         — play arbitrary note sequence (blocking)
 *   play_melody_async()   — spawn task to play melody (non-blocking)
 */

#include "boot_sound.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "music_player.h"
#include "mic_input.h"
#include "screen_settings.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

/* esp_codec_dev — ES8311 driver */
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "sound";

#define MELODY_SAMPLE_RATE  16000

/* ══════════════════════════════════════════════════════════════
 *  Nokia Tune  (Gran Vals by Francisco Tárrega — 13 notes)
 *  Tempo ~220 BPM  →  1/8 = 136ms,  1/4 = 273ms,  1/2 = 545ms
 * ══════════════════════════════════════════════════════════════ */
static const tone_note_t nokia_tune[] = {
    { 659, 125 },   /* E5  */
    { 587, 125 },   /* D5  */
    { 370, 250 },   /* F#4 */
    { 415, 250 },   /* G#4 */
    { 554, 125 },   /* C#5 */
    { 494, 125 },   /* B4  */
    { 294, 250 },   /* D4  */
    { 330, 250 },   /* E4  */
    { 494, 125 },   /* B4  */
    { 440, 125 },   /* A4  */
    { 277, 250 },   /* C#4 */
    { 330, 250 },   /* E4  */
    { 440, 500 },   /* A4  */
};
#define NOKIA_TUNE_LEN  (sizeof(nokia_tune) / sizeof(nokia_tune[0]))

/* ══════════════════════════════════════════════════════════════
 *  Core melody player (blocking)
 * ══════════════════════════════════════════════════════════════ */
void play_melody(const tone_note_t *notes, int count, uint8_t volume)
{
    if (!notes || count <= 0) return;
    if (!io_expander_handle) {
        ESP_LOGW(TAG, "No IO expander — skipping sound");
        return;
    }
    if (music_player_is_active()) {
        ESP_LOGI(TAG, "Music playing — skipping melody");
        return;
    }

    /* 1. Enable PA */
    esp_io_expander_set_level(io_expander_handle, TCA9554_PA_PIN_BIT, 1);
    vTaskDelay(pdMS_TO_TICKS(80));

    /* 2. Get shared I2S TX channel (created by mic_init) */
    i2s_chan_handle_t tx = mic_get_shared_i2s_tx();
    if (!tx) {
        ESP_LOGE(TAG, "No shared I2S TX handle — mic_init not called?");
        return;
    }

    /* 3. ES8311 codec via esp_codec_dev */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_port0_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = ctrl,
        .gpio_if    = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,
        .use_mclk   = true,
        .mclk_div   = 256,
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    if (!codec) {
        ESP_LOGE(TAG, "ES8311 new failed");
        return;  /* TX channel is shared — don't delete it */
    }

    audio_codec_i2s_cfg_t data_cfg = { .port = 0, .tx_handle = tx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec, .data_if = data, .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 1,
        .sample_rate = MELODY_SAMPLE_RATE, .mclk_multiple = 256,
    };
    esp_codec_dev_open(dev, &fs);
    esp_codec_dev_write_reg(dev, 0x32, volume);    /* caller-specified volume */

    /* 4. Generate & play each note */
    const int gap_ms = 20;  /* articulation gap between notes */
    const int max_ms = 600; /* max single note buffer */
    int max_samples  = MELODY_SAMPLE_RATE * max_ms / 1000;
    int16_t *buf = heap_caps_malloc(max_samples * sizeof(int16_t),
                                    MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "Sample buf alloc failed");
        goto cleanup;
    }

    const float two_pi = 2.0f * 3.14159265f;

    for (int n = 0; n < count; n++) {
        uint16_t freq = notes[n].freq_hz;
        uint16_t dur  = notes[n].dur_ms;
        if (dur == 0) continue;

        int nsamp = MELODY_SAMPLE_RATE * dur / 1000;
        if (nsamp > max_samples) nsamp = max_samples;

        if (freq == 0) {
            /* Rest note */
            memset(buf, 0, nsamp * sizeof(int16_t));
        } else {
            float w = two_pi * (float)freq;
            for (int i = 0; i < nsamp; i++) {
                float t = (float)i / (float)MELODY_SAMPLE_RATE;
                float env = 1.0f;
                /* Gentle attack + decay for clean articulation */
                if (i < 80)
                    env = (float)i / 80.0f;
                if (i > nsamp - 80)
                    env = (float)(nsamp - i) / 80.0f;
                buf[i] = (int16_t)(sinf(w * t) * env * 28000.0f);
            }
        }

        size_t written;
        i2s_channel_write(tx, buf, nsamp * sizeof(int16_t),
                          &written, portMAX_DELAY);

        /* Short silence gap for note separation */
        if (n < count - 1 && gap_ms > 0) {
            int gap_samp = MELODY_SAMPLE_RATE * gap_ms / 1000;
            memset(buf, 0, gap_samp * sizeof(int16_t));
            i2s_channel_write(tx, buf, gap_samp * sizeof(int16_t),
                              &written, portMAX_DELAY);
        }
    }

    /* Flush silence */
    memset(buf, 0, 512 * sizeof(int16_t));
    size_t written;
    i2s_channel_write(tx, buf, 512 * sizeof(int16_t), &written, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(150));

    heap_caps_free(buf);

cleanup:
    esp_codec_dev_close(dev);
    esp_codec_dev_delete(dev);
    /* TX channel is shared — don't delete it */

    ESP_LOGI(TAG, "Melody played (%d notes)", count);
}

/* ══════════════════════════════════════════════════════════════
 *  Async wrapper (spawns FreeRTOS task)
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    const tone_note_t *notes;
    int count;
    uint8_t volume;
} melody_arg_t;

static void melody_task(void *arg)
{
    melody_arg_t *m = (melody_arg_t *)arg;
    play_melody(m->notes, m->count, m->volume);
    heap_caps_free(m);
    vTaskDelete(NULL);
}

void play_melody_async(const tone_note_t *notes, int count, uint8_t volume)
{
    if (!notes || count <= 0) return;
    melody_arg_t *m = heap_caps_malloc(sizeof(melody_arg_t), MALLOC_CAP_DEFAULT);
    if (!m) return;
    m->notes = notes;
    m->count = count;
    m->volume = volume;
    xTaskCreatePinnedToCore(melody_task, "melody", 4096, m, 2, NULL, 1);
}

/* ══════════════════════════════════════════════════════════════
 *  Boot sound — Nokia tune
 * ══════════════════════════════════════════════════════════════ */
void boot_sound_play(void)
{
    play_melody(nokia_tune, NOKIA_TUNE_LEN, g_volume);
}
