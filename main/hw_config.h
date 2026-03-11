#ifndef HW_CONFIG_H
#define HW_CONFIG_H

#include "driver/gpio.h"

/*=============================================================
 * Waveshare ESP32-S3-Touch-LCD-3.49  Hardware Pin Definitions
 *=============================================================*/

/* SPI Host for QSPI display */
#define LCD_HOST              SPI3_HOST

/* QSPI display pins (AXS15231B) */
#define PIN_NUM_LCD_CS        GPIO_NUM_9
#define PIN_NUM_LCD_PCLK      GPIO_NUM_10
#define PIN_NUM_LCD_DATA0     GPIO_NUM_11
#define PIN_NUM_LCD_DATA1     GPIO_NUM_12
#define PIN_NUM_LCD_DATA2     GPIO_NUM_13
#define PIN_NUM_LCD_DATA3     GPIO_NUM_14
#define PIN_NUM_LCD_RST       GPIO_NUM_21
#define PIN_NUM_BK_LIGHT      GPIO_NUM_8

/* Touch I2C bus (I2C port 1) */
#define TOUCH_SCL_NUM         GPIO_NUM_18
#define TOUCH_SDA_NUM         GPIO_NUM_17
#define TOUCH_I2C_ADDR        0x3B

/* System I2C bus (I2C port 0) - RTC & IMU */
#define ESP_SCL_NUM           GPIO_NUM_48
#define ESP_SDA_NUM           GPIO_NUM_47

/* I2C device addresses */
#define RTC_I2C_ADDR          0x51    /* PCF85063 */
#define IMU_I2C_ADDR          0x6B    /* QMI8658 */

/*=============================================================
 * Display resolution
 * Native (no rotation): 172 x 640
 * Rotated 90°:          640 x 172
 *=============================================================*/
#define DISP_ROT_90           1
#define DISP_ROT_NONE         0
#define DISP_ROTATION         DISP_ROT_90

#if (DISP_ROTATION == DISP_ROT_90)
  #define LCD_H_RES           640
  #define LCD_V_RES           172
#else
  #define LCD_H_RES           172
  #define LCD_V_RES           640
#endif

#define LCD_NOROT_HRES        172
#define LCD_NOROT_VRES        640
#define LCD_BIT_PER_PIXEL     16

/* DMA and SPIRAM buffer sizes */
#define LVGL_DMA_BUFF_LEN     (LCD_NOROT_HRES * 64 * 2)
#define LVGL_SPIRAM_BUFF_LEN  (LCD_H_RES * LCD_V_RES * 2)

/* LVGL timing */
#define LVGL_TICK_PERIOD_MS   5
#define LVGL_TASK_MAX_DELAY   500
#define LVGL_TASK_MIN_DELAY   5

/*=============================================================
 * PCF85063 RTC Register Definitions
 *=============================================================*/
#define PCF85063_REG_CTRL1    0x00
#define PCF85063_REG_CTRL2    0x01
#define PCF85063_REG_SECONDS  0x04
#define PCF85063_REG_MINUTES  0x05
#define PCF85063_REG_HOURS    0x06
#define PCF85063_REG_DAYS     0x07
#define PCF85063_REG_WEEKDAYS 0x08
#define PCF85063_REG_MONTHS   0x09
#define PCF85063_REG_YEARS    0x0A

/*=============================================================
 * QMI8658 IMU Register Definitions
 *=============================================================*/
#define QMI8658_REG_WHO_AM_I  0x00
#define QMI8658_REG_CTRL1     0x02
#define QMI8658_REG_CTRL2     0x03
#define QMI8658_REG_CTRL3     0x04
#define QMI8658_REG_CTRL7     0x08
#define QMI8658_REG_AX_L      0x35
#define QMI8658_REG_GX_L      0x3B

/*=============================================================
 * I2S Audio Pins (ES8311 codec)
 *=============================================================*/
#define PIN_NUM_I2S_MCLK      GPIO_NUM_7
#define PIN_NUM_I2S_BCLK      GPIO_NUM_15
#define PIN_NUM_I2S_WS        GPIO_NUM_46
#define PIN_NUM_I2S_DOUT      GPIO_NUM_45
#define PIN_NUM_I2S_DIN       GPIO_NUM_6

/* ES8311 audio codec (DAC/speaker) on I2C port 0 */
#define ES8311_I2C_ADDR       0x18

/* ES7210 audio codec (ADC/microphone) on I2C port 0 */
#define ES7210_I2C_ADDR       0x40

/* TCA9554 IO expander audio control pin bit masks
 * (matching IO_EXPANDER_PIN_NUM_x values) */
#define TCA9554_PA_PIN_BIT        (1ULL << 7)   /* Speaker PA enable  */
#define TCA9554_CODEC_PIN_BIT     (1ULL << 6)   /* Codec/mic enable   */

/*=============================================================
 * Credentials & API keys (loaded from secrets.h)
 * Copy secrets.h.example → secrets.h and fill in your values.
 * secrets.h is gitignored — never commit real keys.
 *=============================================================*/
#include "secrets.h"

/*=============================================================
 * Weather Configuration
 *=============================================================*/
#define WEATHER_CITY          "Bengaluru,IN"

/*=============================================================
 * Firmware Version
 *=============================================================*/
#define FW_VERSION            "ESPro"

/*=============================================================
 * HuggingFace AI Model
 *=============================================================*/
#define HF_MODEL              "google/flan-t5-base"

#endif /* HW_CONFIG_H */
