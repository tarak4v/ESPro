# SS_Smallstart

MeowKit-inspired multi-mode smartwatch UI for the **Waveshare ESP32-S3-Touch-LCD-3.49** AMOLED wristband.

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-S3 (QFN56), dual-core 240 MHz, 8 MB PSRAM |
| Display | AXS15231B 3.49" AMOLED, 172×640 native (rotated to 640×172 landscape), QSPI |
| Touch | AXS15231B capacitive touch via I2C (port 1, GPIO 17/18, addr 0x3B) |
| RTC | PCF85063 (I2C port 0, GPIO 47/48, addr 0x51) |
| IMU | QMI8658 6-axis accel+gyro (I2C port 0, addr 0x6B) |
| Backlight | LEDC PWM on GPIO 8 (inverted: 0xFF = off, 0x00 = full) |
| Buttons | BOOT (GPIO 0) — user button; PWR (GPIO 16) — power |
| Flash | 16 MB |

## Features

- **Boot splash screen** — displays "ANERI HIRAL TARAK" with live status of each driver being loaded (I2C, RTC, WiFi, touch)
- **WiFi + NTP** — connects to configured WiFi AP, syncs time via SNTP (`pool.ntp.org` / `time.google.com`), writes IST time to PCF85063 RTC
- **4 screens** with swipe navigation (left/right) and BOOT button cycling:
  1. **Clock** — HH:MM:SS digital display (Montserrat 48, green), date + weekday row, page dots
  2. **Menu** — 6-card horizontal app launcher (BLE, WiFi, LED, Game, Music, Monitor)
I thought ther eius   4. **Settings** — Brightness slider (PWM), chip info, firmware version
- **Timezone** — IST (UTC+5:30)

## Project Structure

```
SS_Smallstart_VS/
├── CMakeLists.txt              Root CMake (ESP-IDF project)
├── partitions.csv              Custom partition table (8 MB factory)
├── sdkconfig.defaults          Kconfig defaults (ESP32-S3, PSRAM, LVGL v8)
├── components/
│   ├── i2c_bsp/                Dual I2C bus driver (port 0: RTC+IMU, port 1: touch)
│   └── lcd_bl_pwm_bsp/        LEDC backlight PWM driver
├── main/
│   ├── main.c                  Entry point, display init, LVGL setup, splash, tasks
│   ├── hw_config.h             All pin definitions, display params, register maps, WiFi creds
│   ├── app_manager.c/h         Screen mode manager (swipe + button navigation)
│   ├── wifi_time.c/h           WiFi STA + SNTP → RTC sync
│   ├── screen_clock.c/h        Clock screen + PCF85063 RTC read/init
│   ├── screen_menu.c/h         App launcher screen
│   ├── screen_compass.c/h      IMU compass screen
│   ├── screen_settings.c/h     Settings screen (brightness, chip info)
│   ├── idf_component.yml       Managed component dependencies
│   └── CMakeLists.txt          Main component source list
└── managed_components/         Auto-downloaded dependencies
    ├── lvgl__lvgl/              LVGL v8.4.0
    ├── espressif__esp_lcd_axs15231b/
    ├── espressif__esp_lcd_touch/
    ├── espressif__esp_io_expander/
    └── espressif__esp_io_expander_tca9554/
```

## Dependencies

| Component | Version |
|---|---|
| ESP-IDF | ≥ 5.1.0 (tested with v5.5.1) |
| LVGL | 8.4.0 |
| espressif/esp_lcd_axs15231b | ^1.0.1 |
| espressif/esp_io_expander_tca9554 | ^2.0.1 |

## Build & Flash

```bash
# Set target (first time only)
idf.py set-target esp32s3

# Build
idf.py build

# Flash (replace COMx with your port)
idf.py -p COMx flash

# Monitor serial output
idf.py -p COMx monitor
```

## WiFi Configuration

Edit `main/hw_config.h`:

```c
#define WIFI_SSID  "Your_SSID"
#define WIFI_PASS  "Your_Password"
```

## Navigation

| Input | Action |
|---|---|
| Swipe left | Next screen |
| Swipe right | Previous screen |
| BOOT button press | Next screen |

## Boot Sequence

1. Display + LVGL initialised
2. Splash screen shown: **"ANERI HIRAL TARAK"**
3. Status line updates as each driver loads:
   - `Loading I2C bus...`
   - `Loading PCF85063 RTC...`
   - `Connecting WiFi...`
   - `Loading touch driver...`
   - `Ready!`
4. Splash holds for minimum 3 seconds total
5. Clock screen starts

## License

Private project — Aneri, Hiral, Tarak.
