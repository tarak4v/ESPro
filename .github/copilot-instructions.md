# ESPro SmartWatch — Copilot Instructions

## Project Overview

ESP32-S3 smartwatch firmware built with **ESP-IDF v5.5.1** and **LVGL v8.4**.
Hardware: Waveshare ESP32-S3-Touch-LCD-3.49 (640×172 AMOLED, QSPI AXS15231B display).

**Language:** C (ESP-IDF framework, FreeRTOS).
**Target:** `esp32s3` — 240 MHz dual-core Xtensa LX7, 16 MB flash, 8 MB Octal PSRAM.

---

## Hardware Map

### Pin Assignments

| Peripheral | Bus | Pins |
|---|---|---|
| **Display** (AXS15231B QSPI) | SPI3_HOST | CS=GPIO9, CLK=GPIO10, D0=GPIO11, D1=GPIO12, D2=GPIO13, D3=GPIO14, RST=GPIO21 |
| **Backlight** (inverted PWM) | LEDC | GPIO8 |
| **I2C Bus 0** (sensors/audio) | I2C port 0 | SDA=GPIO47, SCL=GPIO48 |
| **I2C Bus 1** (touch) | I2C port 1 | SDA=GPIO17, SCL=GPIO18 |
| **I2S Audio** | I2S | MCLK=GPIO7, BCLK=GPIO15, WS=GPIO46, DOUT=GPIO45, DIN=GPIO6 |
| **Boot Button** | GPIO | GPIO0 (active low, internal pull-up) |

### I2C Devices on Bus 0

| Device | Address | Purpose |
|---|---|---|
| PCF85063 RTC | 0x51 | Real-time clock (BCD registers at 0x04–0x0A) |
| QMI8658 IMU | 0x6B | 6-axis accel ±8g / gyro ±512dps, ODR 250Hz |
| ES8311 DAC | 0x18 | Speaker output, volume register 0x32 (0–255) |
| ES7210 ADC | 0x40 | Microphone input |
| TCA9554 IO Expander | (default) | PA enable=bit7, Codec enable=bit6 |

### I2C Bus 1

| Device | Address | Purpose |
|---|---|---|
| AXS15231B Touch | 0x3B | Capacitive touch, custom protocol (cmd 0xb5) |

### Display

- Resolution: **640×172** (landscape, USB connector on right)
- Physical orientation: 172×640 portrait, software-rotated 90° CCW
- Color: RGB565 (16-bit), full refresh every frame
- `LCD_H_RES=640`, `LCD_V_RES=172`, `LCD_NOROT_HRES=172`, `LCD_NOROT_VRES=640`
- Frame buffer: 220 KB in PSRAM (`LVGL_SPIRAM_BUFF_LEN`), DMA buffer: 22 KB

### Memory Layout

| Resource | Size | Notes |
|---|---|---|
| Flash | 16 MB | App partition 8 MB, LittleFS 7 MB |
| PSRAM | 8 MB | Octal 80 MHz, used for LVGL buffers + audio streams |
| NVS | 24 KB | Settings persistence |

### Partition Table (`partitions.csv`)

```
nvs,       data, nvs,     ,  0x6000,
phy_init,  data, phy,     ,  0x1000,
factory,   app,  factory, ,  8M,
storage,   data, littlefs,,  7M,
```

---

## Project Structure

```
main/
├── core/
│   ├── main.c              # app_main(), display/LVGL/IMU init, tasks
│   └── app_manager.c       # Screen state machine, swipe/button nav
├── audio/
│   ├── music_player.c      # JioSaavn streaming (HTTP→MP3→I2S)
│   ├── boot_sound.c        # Nokia boot melody (sine wave synth)
│   └── mic_input.c         # ES7210 mic capture
├── screens/
│   ├── screen_clock.c      # Clock face (RTC, weather, flip clock)
│   ├── screen_menu.c       # App launcher grid + overlays (AI, music, WiFi, BLE, game, macropad)
│   ├── screen_settings.c   # Settings (volume, brightness, theme, toggles)
│   ├── screen_tamafi.c     # Virtual pet
│   ├── screen_wifi_cfg.c   # WiFi provisioning
│   └── screen_assistant.c  # Voice AI assistant (Groq/HuggingFace)
├── services/
│   ├── wifi_time.c         # WiFi connect + NTP + RTC sync
│   ├── weather.c           # OpenWeatherMap background fetch (10 min)
│   ├── sd_log.c            # LittleFS logging (/log/exceptions.txt)
│   ├── game_maze.c         # Tilt marble maze (accel physics)
│   └── macropad.c          # BLE HID keyboard (Meet/Teams)
├── hw_config.h             # All pin defs, I2C addrs, display params
└── idf_component.yml       # Managed deps
components/
├── i2c_bsp/                # I2C bus init + read/write helpers
├── lcd_bl_pwm_bsp/         # Backlight PWM driver
└── minimp3/                # MP3 decoder library
```

---

## Architecture & Patterns

### Task Model (FreeRTOS)

| Task | Core | Priority | Stack | Period | Purpose |
|---|---|---|---|---|---|
| `LVGL` | 0 | 4 | 8192 | ~5 ms | `lv_timer_handler()` rendering |
| `AppMgr` | 1 | 3 | 4096 | 100 ms | Screen updates, swipe/button, IMU |
| *ad-hoc tasks* | any | 2-3 | 2048-4096 | one-shot | WiFi scan, BLE scan, AI request, weather fetch, music stream |

**LVGL thread safety:** All LVGL calls must be wrapped in `lvgl_lock()`/`lvgl_unlock()` (mutex-based). The `app_update_task` acquires the lock before calling any `screen_*_update()`.

### Screen Lifecycle

Every screen follows this pattern:

```c
/* Static LVGL handles */
static lv_obj_t *scr = NULL;
static lv_obj_t *some_label = NULL;

void screen_xxx_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);    /* 640×172 */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(th_bg), 0);

    some_label = lv_label_create(scr);
    lv_label_set_text(some_label, "Hello");
    lv_obj_set_style_text_color(some_label, lv_color_hex(th_text), 0);

    lv_disp_load_scr(scr);
}

void screen_xxx_update(void) {
    /* Called every 100 ms from app_manager_update() */
    /* Read sensors, update labels, poll async results */
    lv_label_set_text_fmt(some_label, "Val: %d", value);
}

void screen_xxx_destroy(void) {
    if (scr) {
        some_label = NULL;   /* Clear refs BEFORE delete */
        lv_obj_del(scr);
        scr = NULL;
    }
}
```

**Rules:**
- `create()` allocates all LVGL objects, applies theme colors (`th_bg`, `th_card`, `th_text`, `th_label`, `th_btn`), ends with `lv_disp_load_scr(scr)`
- `update()` is called at ~100 ms; never allocate/delete objects here, only update existing ones
- `destroy()` nulls all widget pointers, then `lv_obj_del(scr)` deletes the tree
- Screen transition: `create(new)` → `destroy(old)` (managed by `app_manager.c`)

### Screen Navigation (app_manager.c)

```c
typedef enum {
    MODE_CLOCK = 0,     /* Swipe cycle member */
    MODE_MENU = 1,      /* Swipe cycle member */
    MODE_SWIPE_COUNT = 2,
    MODE_SETTINGS = 2,  /* Off-cycle (entered via menu) */
    MODE_TAMAFI = 3,
    MODE_WIFI_CFG = 4,
    MODE_COUNT = 5
} app_mode_t;
```

- **Left swipe** (dx < -40px) → next mode in swipe cycle
- **Right swipe** (dx > +40px) → previous mode
- **BOOT button** (GPIO0, 250 ms debounce) → next mode
- Off-cycle modes: swipe returns to MODE_MENU

### I2C Read/Write (i2c_bsp component)

```c
/* Read register(s) */
uint8_t buf[6];
i2c_read_buff(device_handle, REG_ADDR, buf, len);

/* Write register(s) — NOTE: function is named i2c_writr_buff (typo is intentional) */
uint8_t val = 0x25;
i2c_writr_buff(device_handle, REG_ADDR, &val, 1);
```

**Important:** The write function is `i2c_writr_buff` (not `i2c_write_buff`). This is an existing typo in the BSP — do not "fix" it.

### NVS Settings Pattern

```c
#define NVS_NS  "settings"

/* Read */
nvs_handle_t h;
if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    uint8_t val;
    if (nvs_get_u8(h, "key", &val) == ESP_OK) g_setting = val;
    nvs_close(h);
}

/* Write */
if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "key", new_val);
    nvs_commit(h);
    nvs_close(h);
}
```

### HTTP Client Pattern

```c
esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = 15000,
    .crt_bundle_attach = esp_crt_bundle_attach,  /* TLS cert bundle */
};
esp_http_client_handle_t client = esp_http_client_init(&cfg);
/* Set headers, POST body if needed */
esp_http_client_perform(client);
/* Read response */
esp_http_client_cleanup(client);
```

Always use `esp_crt_bundle_attach` for HTTPS. Never disable certificate verification.

### Audio Pipeline (Music Player)

1. **Fetch** songs from JioSaavn search API → parse JSON with cJSON
2. **Decrypt** media URL: Base64 decode → DES-ECB decrypt (key `"38346591"`) → PKCS#5 unpad
3. **Stream** MP3 via HTTP into a FreeRTOS StreamBuffer
4. **Decode** with minimp3 (`mp3dec_decode_frame`) in a dedicated task
5. **Output** PCM via `i2s_channel_write()` → ES8311 DAC → speaker

Audio init sequence: enable TCA9554 PA bit → configure I2S clock → init ES8311 via `esp_codec_dev`.

### Display Orientation & Flip

The display is physically portrait (172×640) but software-rotated 90° CCW to landscape (640×172).
IMU-based auto-flip: if Z-axis accel < -0.5g (upside-down) for 1 second, the frame buffer is flipped 180°.
Touch coordinates are also remapped when flipped.

---

## Global Settings Variables

Defined in `screen_settings.c`, externed in header:

```c
extern uint8_t  g_volume;         /* 0-255 */
extern bool     g_boot_sound_en;
extern bool     g_clock_24h;
extern bool     g_theme_dark;
extern bool     g_clock_flip;     /* Flip clock animation mode */

/* Theme palette (set by theme_apply()) */
extern uint32_t th_bg;            /* Background: dark=0x0A0A14, light=0xF0F0F5 */
extern uint32_t th_card;          /* Card/panel: dark=0x1A1A2E, light=0xFFFFFF */
extern uint32_t th_text;          /* Primary text: dark=0xFFFFFF, light=0x1A1A2E */
extern uint32_t th_label;         /* Secondary text: dark=0x888888, light=0x666666 */
extern uint32_t th_btn;           /* Button/slider: dark=0x333333, light=0xDDDDDD */
```

Always use theme variables (`th_bg`, `th_text`, etc.) for colors — never hardcode color hex values directly in screens.

---

## LVGL Conventions (v8.4)

- **Fonts:** `lv_font_montserrat_12/14/16/20/28/36/48` (all enabled in sdkconfig)
- **Colors:** Always `lv_color_hex(0xRRGGBB)` — never `lv_color_make(r,g,b)`
- **Positioning:** Prefer `lv_obj_set_pos(obj, x, y)` for absolute layout on this fixed-size screen
- **Alignment:** `lv_obj_align(obj, LV_ALIGN_*, x_ofs, y_ofs)` for relative positioning
- **Styles:** Define `static lv_style_t` in `init_styles()`, apply with `lv_obj_add_style()`
- **Events:** `lv_obj_add_event_cb(obj, callback, LV_EVENT_*, user_data)`
- **Text format:** `lv_label_set_text_fmt(label, "Temp: %.1f°C", temp)` — printf-style
- **Scrolling:** Disabled by default on root screen: `lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE)`
- **Screen display size:** Always 640×172. Design all layouts for this resolution.

### Overlay Pattern (used in screen_menu.c)

Menu apps (Music, Game, AI, etc.) open as overlays on the menu screen:

```c
/* Open overlay */
static lv_obj_t *overlay = NULL;

void app_xxx_open(lv_obj_t *parent) {
    overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(overlay, 0, 0);
    /* Create overlay content */
}

void app_xxx_close(void) {
    if (overlay) { lv_obj_del(overlay); overlay = NULL; }
}

void app_xxx_update(void) {
    if (!overlay) return;
    /* Update overlay content every 100 ms */
}
```

---

## Build & Flash

```bash
# Set up ESP-IDF environment (required in every new terminal)
. $IDF_PATH/export.sh          # Linux/Mac
. $env:IDF_PATH\export.ps1    # Windows PowerShell

# Build
idf.py build

# Flash (USB serial)
idf.py -p COM_PORT flash

# Monitor serial output
idf.py -p COM_PORT monitor

# Full erase + flash (needed after partition table changes)
idf.py -p COM_PORT erase-flash flash

# Menuconfig (modify sdkconfig)
idf.py menuconfig
```

---

## Coding Rules

1. **C only** — no C++. Use ESP-IDF APIs and FreeRTOS primitives.
2. **Memory allocation:** Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for large buffers (>1 KB). Use `MALLOC_CAP_DMA` for DMA-accessible buffers. Internal RAM is precious (~320 KB).
3. **Logging:** Use `ESP_LOGI(TAG, ...)`, `ESP_LOGW(TAG, ...)`, `ESP_LOGE(TAG, ...)`. Define `static const char *TAG = "module_name";` at the top of each file.
4. **Error handling:** Check return values of ESP-IDF calls. Use `ESP_ERROR_CHECK()` only during init. In runtime code, check and log errors gracefully.
5. **Task stack sizes:** Minimum 2048 words. Use 4096 for tasks with HTTP/TLS. Use 8192 for LVGL rendering.
6. **Delays:** `vTaskDelay(pdMS_TO_TICKS(ms))` — never busy-wait.
7. **Concurrency:** LVGL is NOT thread-safe. Always acquire `lvgl_lock()` before any `lv_*` call outside the LVGL task. Release with `lvgl_unlock()`.
8. **Strings:** Use `snprintf()` — never `sprintf()`. Always bound buffer sizes.
9. **Naming:** Functions: `module_action()` (e.g., `weather_init()`, `screen_clock_create()`). Globals: `g_name`. Statics: `s_name` or no prefix.
10. **Includes:** Use `"local.h"` for project files, `<esp_xxx.h>` for ESP-IDF, `<freertos/xxx.h>` for FreeRTOS.
11. **Secrets:** Never hardcode credentials. Use defines from `hw_config.h` (or a `secrets.h` not committed to git).
12. **PSRAM strings:** When allocating large response buffers (HTTP, JSON, audio), use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
13. **Timezone:** IST is set globally: `setenv("TZ", "IST-5:30", 1)`. Use `localtime()` for formatted time.
14. **BCD conversions:** RTC uses BCD encoding. Helper: `bcd2dec(val) = (val >> 4) * 10 + (val & 0x0F)`.

## Dependencies (idf_component.yml)

```yaml
espressif/esp_lcd_axs15231b: ^1.0.1
espressif/esp_io_expander_tca9554: ^2.0.1
lvgl/lvgl: ^8.4.0
espressif/esp_codec_dev: ^1.5.4
espressif/esp_audio_codec: ^2.4.1
```

When adding new ESP-IDF managed components, add them to `main/idf_component.yml` and `PRIV_REQUIRES` in `main/CMakeLists.txt`.

## Common Pitfalls

- **Display is 640×172** — not 320×240 or other common sizes. All UI layouts must fit this elongated widescreen.
- **I2C write function typo:** It's `i2c_writr_buff()`, not `i2c_write_buff()`. Don't "correct" it.
- **LVGL v8.4** — not v9. Use v8 API (`lv_disp_drv_t`, `lv_indev_drv_t`, `lv_disp_load_scr()`). Do not use v9 APIs.
- **SPI3_HOST is taken** — the display uses it. Use SPI2_HOST if adding SPI peripherals.
- **No SD card slot** — the board has no SD card. Use LittleFS on internal flash for file storage (7 MB partition).
- **Backlight is inverted** — duty 0 = full brightness, 255 = off.
- **Audio requires PA enable** — before any audio output, enable the TCA9554 PA pin (bit 7). Disable after to save power.
- **QMI8658 accel scale** — ±8g range = 4096 LSB/g. Convert: `float g = (int16_t)raw / 4096.0f`.
- **PCF85063 RTC** — registers are BCD-encoded. Always use `bcd2dec()`/`dec2bcd()` helpers.
- **Full-screen refresh** — `disp_drv.full_refresh = 1`. Don't try partial refresh with this display setup.
