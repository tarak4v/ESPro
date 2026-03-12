/**
 * @file mic_input.c
 * @brief ES7210 ADC microphone input via I2S RX.
 *
 * Reads audio from the on-board ES7210 4-ch ADC (using channel 1),
 * computes RMS level, and exposes it as a 0–100 scale.
 *
 * Creates I2S_NUM_0 in full-duplex mode (TX for speaker, RX for mic).
 * The shared TX handle is used by boot_sound and music_player.
 */

#include "mic_input.h"
#include "hw_config.h"
#include "i2c_bsp.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mic";

#define MIC_SAMPLE_RATE     16000
#define MIC_BUF_SAMPLES     512       /* ~32 ms at 16 kHz */
#define MIC_READ_BYTES      (MIC_BUF_SAMPLES * 2)   /* 16-bit mono */

/* ── State ──────────────────────────────────────────────── */
static i2s_chan_handle_t      s_shared_tx = NULL;  /* TX for speaker (shared) */
static i2s_chan_handle_t      s_rx        = NULL;  /* RX for microphone */
static esp_codec_dev_handle_t s_mic_dev  = NULL;
static TaskHandle_t           s_task     = NULL;
static volatile bool          s_running  = false;
static volatile uint8_t       s_level    = 0;

/* ── Recording buffer (optional, set by screen_assistant) ── */
static int16_t               *s_rec_buf  = NULL;
static size_t                 s_rec_max  = 0;   /* max bytes */
static volatile size_t        s_rec_pos  = 0;   /* bytes written */

/* ── RMS → 0..100 mapping ──────────────────────────────── */
static uint8_t rms_to_level(float rms)
{
    /* ES7210 with 30 dB gain: quiet room ≈ 10–30, voice ≈ 100–2000+.
       Map with log scale: level = 40 * log10(rms / 5), clamped 0..100 */
    if (rms < 5.0f) return 0;
    float db = 40.0f * log10f(rms / 5.0f);
    if (db < 0) return 0;
    if (db > 100) return 100;
    return (uint8_t)db;
}

/* ── Reader task ────────────────────────────────────────── */
static void mic_task(void *arg)
{
    (void)arg;
    int16_t *buf = heap_caps_malloc(MIC_READ_BYTES, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to alloc mic buffer");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Mic reader task started");
    int read_count = 0;
    int err_count = 0;

    while (s_running) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, MIC_READ_BYTES,
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            err_count++;
            if (err_count <= 5) {
                ESP_LOGW(TAG, "i2s_channel_read err=%d bytes=%u", err, (unsigned)bytes_read);
            }
            continue;
        }

        read_count++;

        /* Copy to recording buffer if active */
        if (s_rec_buf && s_rec_pos + bytes_read <= s_rec_max) {
            memcpy((uint8_t *)s_rec_buf + s_rec_pos, buf, bytes_read);
            s_rec_pos += bytes_read;
        }

        /* Compute RMS */
        int samples = bytes_read / 2;
        int64_t sum_sq = 0;
        for (int i = 0; i < samples; i++) {
            int32_t s = buf[i];
            sum_sq += s * s;
        }
        float rms = sqrtf((float)sum_sq / samples);
        s_level = rms_to_level(rms);

        /* Log every ~1 second (16000 Hz / 512 samples = ~31 reads/sec) */
        if (read_count % 31 == 1) {
            ESP_LOGI(TAG, "mic: reads=%d rms=%.0f level=%u rec_pos=%u/%u",
                     read_count, rms, s_level,
                     (unsigned)s_rec_pos, (unsigned)s_rec_max);
        }
    }

    free(buf);
    ESP_LOGI(TAG, "Mic reader task stopped");
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────── */
void mic_init(void)
{
    if (s_rx) return;   /* already inited */

    /* Enable mic codec via IO expander (TCA9554 pin 6) */
    if (io_expander_handle) {
        esp_io_expander_set_level(io_expander_handle, TCA9554_CODEC_PIN_BIT, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ── I2S_NUM_0 full-duplex (TX for speaker + RX for mic) ── */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &s_shared_tx, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S full-duplex channel create failed");
        return;
    }

    /* Initialise TX channel (speaker output) */
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
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
    tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(s_shared_tx, &tx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX init failed");
        i2s_del_channel(s_shared_tx); s_shared_tx = NULL;
        i2s_del_channel(s_rx); s_rx = NULL;
        return;
    }

    /* Initialise RX channel (microphone input)
     * In full-duplex mode, shared clock pins (MCLK/BCLK/WS) are already
     * configured by the TX channel — RX must use I2S_GPIO_UNUSED for them. */
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws   = I2S_GPIO_UNUSED,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_NUM_I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(s_rx, &rx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX init failed");
        i2s_del_channel(s_shared_tx); s_shared_tx = NULL;
        i2s_del_channel(s_rx); s_rx = NULL;
        return;
    }

    /* ES7210 codec setup */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_port0_bus_handle,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1,
        .mclk_src    = ES7210_MCLK_FROM_PAD,
        .mclk_div    = 256,
    };
    const audio_codec_if_t *codec = es7210_codec_new(&es_cfg);
    if (!codec) {
        ESP_LOGE(TAG, "ES7210 codec init failed");
        i2s_del_channel(s_shared_tx); s_shared_tx = NULL;
        i2s_del_channel(s_rx); s_rx = NULL;
        return;
    }

    audio_codec_i2s_cfg_t data_cfg = {
        .port      = 0,
        .rx_handle = s_rx,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec,
        .data_if  = data,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    s_mic_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "Mic codec dev create failed");
        i2s_del_channel(s_shared_tx); s_shared_tx = NULL;
        i2s_del_channel(s_rx); s_rx = NULL;
        return;
    }

    /* NOTE: esp_codec_dev_open() is deferred to mic_start() so the
     * RX channel stays in READY state until needed. */

    ESP_LOGI(TAG, "Mic initialised (ES7210, I2S_NUM_0 full-duplex, %d Hz)", MIC_SAMPLE_RATE);
}

i2s_chan_handle_t mic_get_shared_i2s_tx(void)
{
    return s_shared_tx;
}

void mic_start(void)
{
    if (s_running || !s_rx) return;

    /* Restore shared I2S clock to mic sample rate and enable TX.
     * TX owns the clock pins (MCLK/BCLK/WS) — it must be enabled for
     * the I2S peripheral to generate clocks that the ES7210 ADC needs.
     * TTS or music playback may have left TX at a different rate. */
    if (s_shared_tx) {
        i2s_channel_disable(s_shared_tx);   /* ensure READY state for reconfig */
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
        clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        i2s_channel_reconfig_std_clock(s_shared_tx, &clk);
        i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        i2s_channel_reconfig_std_slot(s_shared_tx, &slot);
        i2s_channel_enable(s_shared_tx);    /* start clocks for RX */
    }

    /* Open codec dev → configures ES7210 + enables RX channel */
    if (s_mic_dev) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = 1,
            .sample_rate     = MIC_SAMPLE_RATE,
        };
        esp_err_t ret = esp_codec_dev_open(s_mic_dev, &fs);
        ESP_LOGI(TAG, "esp_codec_dev_open => %s", esp_err_to_name(ret));
        /* Set microphone input gain (30 dB) */
        if (ret == ESP_OK) {
            esp_codec_dev_set_in_gain(s_mic_dev, 30.0);
            ESP_LOGI(TAG, "Mic input gain set to 30 dB");
        }
    } else {
        ESP_LOGE(TAG, "s_mic_dev is NULL — ES7210 not initialised!");
    }

    s_running = true;
    s_level   = 0;

    xTaskCreate(mic_task, "mic_read", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "Mic started");
}

void mic_stop(void)
{
    if (!s_running) return;
    s_running = false;

    /* Task will self-delete after loop exits */
    vTaskDelay(pdMS_TO_TICKS(200));
    s_task = NULL;

    /* Close codec dev → puts ES7210 in standby + disables RX */
    if (s_mic_dev) {
        esp_codec_dev_close(s_mic_dev);
    }

    /* Disable TX (clock source) — consumers will re-enable as needed */
    if (s_shared_tx) {
        i2s_channel_disable(s_shared_tx);
    }
    s_level = 0;

    ESP_LOGI(TAG, "Mic stopped");
}

bool mic_is_active(void)
{
    return s_running;
}

uint8_t mic_get_level(void)
{
    return s_level;
}

void mic_set_record_buffer(int16_t *buf, size_t max_bytes)
{
    s_rec_buf = buf;
    s_rec_max = max_bytes;
    s_rec_pos = 0;
}

size_t mic_get_recorded_bytes(void)
{
    return s_rec_pos;
}

void mic_clear_record_buffer(void)
{
    s_rec_buf = NULL;
    s_rec_max = 0;
    s_rec_pos = 0;
}
