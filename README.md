# ESPro Smartwatch

Multi-mode smartwatch firmware for the **Waveshare ESP32-S3-Touch-LCD-3.49** AMOLED wristband, built with ESP-IDF and LVGL.

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

- **Boot splash screen** — displays "ANERI HIRAL TARAK" with live status of each driver being loaded (I2C, RTC, WiFi, touch) and boot sound
- **WiFi + NTP** — connects to configured WiFi AP, syncs time via SNTP (`pool.ntp.org` / `time.google.com`), writes IST time to PCF85063 RTC
- **Swipe navigation** (left/right) and BOOT button cycling between screens:
  1. **Clock** — HH:MM:SS digital display (Montserrat 48, green), date + weekday row, page dots
  2. **Menu** — app launcher cards (Macropad, Weather, Music, Game, Compass, Monitor, etc.)
  3. **Compass** — QMI8658 IMU-based compass display
  4. **Settings** — Brightness slider (PWM), chip info, firmware version
- **Macropad** — BLE HID + WiFi UDP meeting control panel for Google Meet & MS Teams
  - Mic / Video / Volume / End Meeting controls
  - Meet/Teams app selector, BLE/WiFi mode toggle
  - PC↔ESP32 bidirectional state sync via UDP
  - Long-press to manually sync button visual state
- **Weather** — fetches weather data via HTTP
- **Music Player** — MP3 playback via minimp3 decoder + I2S audio
- **Maze Game** — touch-controlled maze game
- **SD Logging** — data logging to SD card via SPI
- **Timezone** — IST (UTC+5:30)

## Project Structure

```
SS_Smallstart_VS/
├── CMakeLists.txt              Root CMake (ESP-IDF project)
├── partitions.csv              Custom partition table (8 MB factory)
├── sdkconfig.defaults          Kconfig defaults (ESP32-S3, PSRAM, LVGL v8)
├── components/
│   ├── i2c_bsp/                Dual I2C bus driver (port 0: RTC+IMU, port 1: touch)
│   ├── lcd_bl_pwm_bsp/        LEDC backlight PWM driver
│   └── minimp3/               MP3 decoder (auto-downloaded from GitHub)
├── main/
│   ├── main.c                  Entry point, display init, LVGL setup, splash, tasks
│   ├── hw_config.h             All pin definitions, display params, register maps, WiFi creds
│   ├── app_manager.c/h         Screen mode manager (swipe + button navigation)
│   ├── wifi_time.c/h           WiFi STA + SNTP → RTC sync
│   ├── screen_clock.c/h        Clock screen + PCF85063 RTC read/init
│   ├── screen_menu.c/h         App launcher screen
│   ├── screen_compass.c/h      IMU compass screen
│   ├── screen_settings.c/h     Settings screen (brightness, chip info)
│   ├── macropad.c/h            BLE HID + WiFi UDP macropad (meeting controls)
│   ├── music_player.c/h        MP3 music player
│   ├── game_maze.c/h           Maze game
│   ├── weather.c/h             Weather display via HTTP
│   ├── boot_sound.c/h          Boot-up sound playback
│   ├── sd_log.c/h              SD card data logging
│   ├── idf_component.yml       Managed component dependencies
│   └── CMakeLists.txt          Main component source list
├── tools/
│   ├── macropad_listener.py    PC WiFi listener for macropad (bidirectional state sync)
│   └── pc_monitor.py           PC system monitor (CPU/RAM/GPU stats via UDP)
└── managed_components/         Auto-downloaded dependencies
    ├── lvgl__lvgl/
    ├── espressif__esp_lcd_axs15231b/
    ├── espressif__esp_lcd_touch/
    ├── espressif__esp_io_expander/
    ├── espressif__esp_io_expander_tca9554/
    └── espressif__cmake_utilities/
```

## Dependencies

| Component | Version | Type |
|---|---|---|
| ESP-IDF | v5.5.1 | Framework |
| LVGL | 8.4.0 | Managed component |
| espressif/esp_lcd_axs15231b | 1.0.1 | Managed component |
| espressif/esp_io_expander_tca9554 | 2.0.3 | Managed component |
| espressif/esp_io_expander | 1.2.0 | Managed (transitive) |
| espressif/esp_lcd_touch | 1.2.1 | Managed (transitive) |
| espressif/cmake_utilities | 0.5.3 | Managed (transitive) |
| minimp3 | latest | Local (auto-downloaded) |

### Python Tools (PC side)

```bash
pip install pyautogui psutil gputil
```

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

## Macropad Usage

The macropad overlay supports two transport modes:

- **BLE HID** — pairs directly with PC as a keyboard (no dongle needed)
- **WiFi UDP** — sends commands to PC via UDP broadcast; run the listener on PC:
  ```bash
  python tools/macropad_listener.py --app meet
  ```
  The listener executes keyboard shortcuts and sends mic/video state back to the watch.

| Button | Google Meet | MS Teams |
|---|---|---|
| Mic toggle | Ctrl+D | Ctrl+Shift+M |
| Video toggle | Ctrl+E | Ctrl+Shift+O |
| End meeting | Ctrl+W | Ctrl+Shift+H |
| Long-press Mic/Video | Sync visual state (no keystroke) | Same |

## Boot Sequence

1. Display + LVGL initialised
2. Splash screen shown: **"ANERI HIRAL TARAK"**
3. Status line updates as each driver loads:
   - `Loading I2C bus...`
   - `Loading PCF85063 RTC...`
   - `Connecting WiFi...`
   - `Loading touch driver...`
   - `Ready!`
4. Boot sound plays
5. Splash holds for minimum 3 seconds total
6. Clock screen starts

## Credits & Acknowledgments

This project is built upon the following open-source projects. Thank you to all the maintainers and contributors!

### Framework

| Project | License | Link |
|---|---|---|
| **ESP-IDF** — Espressif IoT Development Framework | Apache 2.0 | [github.com/espressif/esp-idf](https://github.com/espressif/esp-idf) |

### Graphics & UI

| Project | License | Link |
|---|---|---|
| **LVGL** — Light and Versatile Graphics Library | MIT | [github.com/lvgl/lvgl](https://github.com/lvgl/lvgl) |

### Espressif Components (via [ESP Component Registry](https://components.espressif.com/))

| Project | Description | Link |
|---|---|---|
| **esp_lcd_axs15231b** | LCD & touch driver for AXS15231B AMOLED | [github.com/espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution) |
| **esp_lcd_touch** | Base LCD touch controller abstraction | [github.com/espressif/esp-bsp](https://github.com/espressif/esp-bsp) |
| **esp_io_expander** | IO expander base abstraction | [github.com/espressif/esp-bsp](https://github.com/espressif/esp-bsp) |
| **esp_io_expander_tca9554** | TCA9554 I²C IO expander driver | [github.com/espressif/esp-bsp](https://github.com/espressif/esp-bsp) |
| **cmake_utilities** | CMake utility functions for ESP-IDF | [github.com/espressif/esp-iot-solution](https://github.com/espressif/esp-iot-solution) |

### Audio

| Project | License | Link |
|---|---|---|
| **minimp3** — Single-header MP3 decoder | CC0 1.0 (Public Domain) | [github.com/lieff/minimp3](https://github.com/lieff/minimp3) |

### Python Tools (PC companion scripts)

| Project | License | Link |
|---|---|---|
| **PyAutoGUI** — Cross-platform GUI automation | BSD 3-Clause | [github.com/asweigart/pyautogui](https://github.com/asweigart/pyautogui) |
| **psutil** — Cross-platform process & system monitoring | BSD 3-Clause | [github.com/giampaolo/psutil](https://github.com/giampaolo/psutil) |
| **GPUtil** — GPU monitoring utility | MIT | [github.com/anderskm/gputil](https://github.com/anderskm/gputil) |

### Hardware Reference

| Resource | Link |
|---|---|
| **Waveshare ESP32-S3-Touch-LCD-3.49** Wiki | [waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49) |

## License

Private project — Tarak.
