/**
 * @file boot_sound.c
 * @brief Boot "ting" sound via ES8311 codec + I2S + TCA9554 PA.
 *
 * Generates a 1 kHz decaying sine-wave tone (~300 ms) and plays it
 * through the on-board ES8311 DAC → power amplifier → speaker.
 */

#include "boot_sound.h"
#include "hw_config.h"
#include "i2c_bsp.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "sound";

/* Tone parameters */
#define TONE_FREQ_HZ       1000
#define TONE_SAMPLE_RATE   16000
#define TONE_DURATION_MS   600
#define TONE_NUM_SAMPLES   (TONE_SAMPLE_RATE * TONE_DURATION_MS / 1000)

/* ── ES8311 register helpers ──────────────────────────────── */

static void es8311_write_reg(uint8_t reg, uint8_t val)
{
    i2c_writr_buff(es8311_dev_handle, reg, &val, 1);
}

static void es8311_codec_init(void)
{
    /* Soft reset */
    es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_write_reg(0x00, 0x00);

    /* Clock configuration: MCLK from pad, MCLK/LRCK = 256 */
    es8311_write_reg(0x01, 0x30);
    es8311_write_reg(0x02, 0x00);
    es8311_write_reg(0x03, 0x10);
    es8311_write_reg(0x04, 0x10);
    es8311_write_reg(0x05, 0x00);
    es8311_write_reg(0x06, 0x03);
    es8311_write_reg(0x07, 0x00);
    es8311_write_reg(0x08, 0xFF);

    /* I2S format: Philips standard, 16-bit */
    es8311_write_reg(0x09, 0x0C);
    es8311_write_reg(0x0A, 0x0C);

    /* System */
    es8311_write_reg(0x0B, 0x00);
    es8311_write_reg(0x0C, 0x00);

    /* ADC off (not needed for playback) */
    es8311_write_reg(0x10, 0x1F);
    es8311_write_reg(0x11, 0x7F);

    /* Power up analog circuitry */
    es8311_write_reg(0x00, 0x80);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Start DAC */
    es8311_write_reg(0x0D, 0x01);
    es8311_write_reg(0x12, 0x00);
    es8311_write_reg(0x13, 0x10);
    es8311_write_reg(0x14, 0x1A);

    /* DAC volume (max) */
    es8311_write_reg(0x32, 0xFF);

    ESP_LOGI(TAG, "ES8311 codec initialised");
}

/* ── Public API ───────────────────────────────────────────── */

void boot_sound_play(void)
{
    /* PA is already enabled by i2c_bsp via shared TCA9554 handle.
     * If not available, skip boot sound gracefully. */
    if (!io_expander_handle) {
        ESP_LOGW(TAG, "No IO expander — skipping boot sound");
        return;
    }

    /* 1. Initialise ES8311 codec */
    es8311_codec_init();

    /* 2. Ensure PA is on (should already be from i2c_bsp init) */
    esp_io_expander_set_level(io_expander_handle, TCA9554_PA_PIN_BIT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 4. Configure I2S TX channel */
    i2s_chan_handle_t tx_chan = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_NUM_I2S_MCLK,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws   = PIN_NUM_I2S_WS,
            .dout = PIN_NUM_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    /* 5. Generate decaying sine-wave "ting" */
    int16_t *samples = heap_caps_malloc(TONE_NUM_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_DEFAULT);
    if (samples) {
        const float two_pi_f = 2.0f * 3.14159265f * TONE_FREQ_HZ;
        for (int i = 0; i < TONE_NUM_SAMPLES; i++) {
            float t = (float)i / (float)TONE_SAMPLE_RATE;
            float envelope = expf(-3.0f * t);
            samples[i] = (int16_t)(sinf(two_pi_f * t) * envelope * 32000.0f);
        }

        size_t bytes_written;
        i2s_channel_write(tx_chan, samples,
                          TONE_NUM_SAMPLES * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);

        /* Flush silence through DMA to ensure all audio is played */
        memset(samples, 0, 512 * sizeof(int16_t));
        i2s_channel_write(tx_chan, samples, 512 * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(200));   /* let final samples play out */

        heap_caps_free(samples);
    }

    /* 6. Tear down I2S (PA stays on — managed by i2c_bsp) */
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);

    ESP_LOGI(TAG, "Boot ting played");
}
