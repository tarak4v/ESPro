#include <stdio.h>
#include "i2c_bsp.h"
#include "hw_config.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG_I2C = "i2c_bsp";

static i2c_master_bus_handle_t user_i2c_port0_handle = NULL;
static i2c_master_bus_handle_t user_i2c_port1_handle = NULL;

i2c_master_dev_handle_t disp_touch_dev_handle = NULL;
i2c_master_dev_handle_t rtc_dev_handle = NULL;
i2c_master_dev_handle_t imu_dev_handle = NULL;
i2c_master_dev_handle_t es8311_dev_handle = NULL;
i2c_master_dev_handle_t es7210_dev_handle = NULL;
i2c_master_bus_handle_t i2c_port0_bus_handle = NULL;
esp_io_expander_handle_t io_expander_handle = NULL;

static uint32_t i2c_data_pdMS_TICKS = 0;
static uint32_t i2c_done_pdMS_TICKS = 0;

void i2c_master_Init(void)
{
    i2c_data_pdMS_TICKS = pdMS_TO_TICKS(5000);
    i2c_done_pdMS_TICKS = pdMS_TO_TICKS(1000);

    /* I2C port 0: RTC + IMU (GPIO47/48) */
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = ESP_SCL_NUM,
        .sda_io_num = ESP_SDA_NUM,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &user_i2c_port0_handle));
    i2c_port0_bus_handle = user_i2c_port0_handle;

    /* I2C port 1: Touch (GPIO17/18) */
    i2c_bus_config.scl_io_num = TOUCH_SCL_NUM;
    i2c_bus_config.sda_io_num = TOUCH_SDA_NUM;
    i2c_bus_config.i2c_port = I2C_NUM_1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &user_i2c_port1_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 300000,
    };

    /* Add RTC device */
    dev_cfg.device_address = RTC_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &rtc_dev_handle));

    /* Add IMU device */
    dev_cfg.device_address = IMU_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &imu_dev_handle));

    /* Add ES8311 audio codec (DAC/speaker) */
    dev_cfg.device_address = ES8311_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &es8311_dev_handle));

    /* Add ES7210 audio codec (ADC/microphone) */
    dev_cfg.device_address = ES7210_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port0_handle, &dev_cfg, &es7210_dev_handle));

    /* Add Touch device */
    dev_cfg.device_address = TOUCH_I2C_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(user_i2c_port1_handle, &dev_cfg, &disp_touch_dev_handle));

    /* TCA9554 IO expander — enable PA (PIN 7) + codec/mic (PIN 6) */
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(
        i2c_port0_bus_handle,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
        &io_expander_handle);
    if (ret == ESP_OK) {
        esp_io_expander_set_dir(io_expander_handle,
            TCA9554_PA_PIN_BIT | TCA9554_CODEC_PIN_BIT,
            IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander_handle,
            TCA9554_PA_PIN_BIT | TCA9554_CODEC_PIN_BIT, 1);
        ESP_LOGI(TAG_I2C, "TCA9554: PA + codec pins enabled");
    } else {
        ESP_LOGW(TAG_I2C, "TCA9554 init failed: %s", esp_err_to_name(ret));
    }
}

uint8_t i2c_writr_buff(i2c_master_dev_handle_t dev_handle, int reg,
                        uint8_t *buf, uint8_t len)
{
    uint8_t ret;
    uint8_t *pbuf = NULL;

    ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle, i2c_done_pdMS_TICKS);
    if (ret != ESP_OK) return ret;

    if (reg == -1) {
        ret = i2c_master_transmit(dev_handle, buf, len, i2c_data_pdMS_TICKS);
    } else {
        pbuf = (uint8_t *)malloc(len + 1);
        pbuf[0] = reg;
        for (uint8_t i = 0; i < len; i++) {
            pbuf[i + 1] = buf[i];
        }
        ret = i2c_master_transmit(dev_handle, pbuf, len + 1, i2c_data_pdMS_TICKS);
        free(pbuf);
    }
    return ret;
}

uint8_t i2c_master_write_read_dev(i2c_master_dev_handle_t dev_handle,
                                   uint8_t *writeBuf, uint8_t writeLen,
                                   uint8_t *readBuf, uint8_t readLen)
{
    uint8_t ret;
    ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle, i2c_done_pdMS_TICKS);
    if (ret != ESP_OK) return ret;

    ret = i2c_master_transmit_receive(dev_handle, writeBuf, writeLen,
                                       readBuf, readLen, i2c_data_pdMS_TICKS);
    return ret;
}

uint8_t i2c_read_buff(i2c_master_dev_handle_t dev_handle, int reg,
                       uint8_t *buf, uint8_t len)
{
    uint8_t ret;
    uint8_t addr = 0;

    ret = i2c_master_bus_wait_all_done(user_i2c_port0_handle, i2c_done_pdMS_TICKS);
    if (ret != ESP_OK) return ret;

    if (reg == -1) {
        ret = i2c_master_receive(dev_handle, buf, len, i2c_data_pdMS_TICKS);
    } else {
        addr = (uint8_t)reg;
        ret = i2c_master_transmit_receive(dev_handle, &addr, 1,
                                           buf, len, i2c_data_pdMS_TICKS);
    }
    return ret;
}

uint8_t i2c_master_touch_write_read(i2c_master_dev_handle_t dev_handle,
                                     uint8_t *writeBuf, uint8_t writeLen,
                                     uint8_t *readBuf, uint8_t readLen)
{
    uint8_t ret;
    ret = i2c_master_bus_wait_all_done(user_i2c_port1_handle, i2c_done_pdMS_TICKS);
    if (ret != ESP_OK) return ret;

    ret = i2c_master_transmit_receive(dev_handle, writeBuf, writeLen,
                                       readBuf, readLen, i2c_data_pdMS_TICKS);
    return ret;
}
