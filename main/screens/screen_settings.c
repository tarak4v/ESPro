/**
 * @file screen_settings.c
 * @brief Settings screen — brightness, volume, boot-sound toggle,
 *        12h/24h toggle, device info, reboot.
 *
 * Designed for 640×172 landscape AMOLED.
 * Persistent settings stored in NVS namespace "settings".
 */

#include "screen_settings.h"
#include "app_manager.h"
#include "screen_menu.h"
#include "hw_config.h"
#include "i2c_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "wifi_time.h"
#include "music_player.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";

static lv_obj_t *scr = NULL;
static lv_obj_t *vol_pct_lbl = NULL;

/* ── Shared settings (read by other modules) ──────────────── */
uint8_t  g_volume        = 200;     /* ES8311 DAC reg 0x32: 0-255 */
bool     g_boot_sound_en = true;
bool     g_clock_24h     = false;
bool     g_theme_dark    = true;     /* true = dark (default) */
bool     g_clock_flip    = false;    /* false = digital (default) */

/* ── Theme palette ────────────────────────────────────────── */
uint32_t th_bg   = 0x0A0A14;
uint32_t th_card = 0x1A1A2E;
uint32_t th_text = 0xFFFFFF;
uint32_t th_label = 0x888888;
uint32_t th_btn  = 0x333333;

void theme_apply(void)
{
    if (g_theme_dark) {
        th_bg = 0x0A0A14; th_card = 0x1A1A2E; th_text = 0xFFFFFF;
        th_label = 0x888888; th_btn = 0x333333;
    } else {
        th_bg = 0xF0F0F5; th_card = 0xFFFFFF; th_text = 0x1A1A2E;
        th_label = 0x666666; th_btn = 0xDDDDDD;
    }
}

/* ── NVS helpers ──────────────────────────────────────────── */
#define NVS_NS  "settings"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "volume",  &v) == ESP_OK) g_volume        = v;
        if (nvs_get_u8(h, "bootsnd", &v) == ESP_OK) g_boot_sound_en = v;
        if (nvs_get_u8(h, "clk24h",  &v) == ESP_OK) g_clock_24h     = v;
        if (nvs_get_u8(h, "theme",   &v) == ESP_OK) g_theme_dark    = v;
        if (nvs_get_u8(h, "clkflip", &v) == ESP_OK) g_clock_flip    = v;
        nvs_close(h);
    }
    theme_apply();
}

void settings_load_from_nvs(void) { settings_load(); }

static void settings_save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── ES8311 volume write ──────────────────────────────────── */
static void es8311_set_volume(uint8_t vol)
{
    uint8_t data = vol;
    i2c_writr_buff(es8311_dev_handle, 0x32, &data, 1);
}

/* ── Callbacks ────────────────────────────────────────────── */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    uint16_t duty = (uint16_t)(255 - lv_slider_get_value(s));
    setUpduty(duty);
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    g_volume = (uint8_t)lv_slider_get_value(s);

    /* Apply volume to the active audio path */
    if (music_player_is_active()) {
        music_player_set_volume(g_volume);
    } else {
        es8311_set_volume(g_volume);
    }

    settings_save_u8("volume", g_volume);
    if (vol_pct_lbl)
        lv_label_set_text_fmt(vol_pct_lbl, "%d%%", g_volume * 100 / 255);
}

static void boot_sound_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_boot_sound_en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save_u8("bootsnd", g_boot_sound_en ? 1 : 0);
}

static void clock_24h_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_clock_24h = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save_u8("clk24h", g_clock_24h ? 1 : 0);
}

static void theme_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_theme_dark = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save_u8("theme", g_theme_dark ? 1 : 0);
    theme_apply();
    /* Recreate settings screen to apply now */
    screen_settings_destroy();
    screen_settings_create();
}

static void clock_flip_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_clock_flip = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_save_u8("clkflip", g_clock_flip ? 1 : 0);
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    app_manager_set_mode(MODE_MENU);
}

static void wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    app_manager_set_mode(MODE_WIFI_CFG);
}

static void ble_btn_cb(lv_event_t *e)
{
    (void)e;
    screen_menu_request_ble();
    app_manager_set_mode(MODE_MENU);
}

/* ── Styles ───────────────────────────────────────────────── */
static lv_style_t style_bg, style_label, style_value;

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_label);
    lv_style_set_text_color(&style_label, lv_color_hex(th_label));
    lv_style_set_text_font(&style_label, &lv_font_montserrat_12);

    lv_style_init(&style_value);
    lv_style_set_text_color(&style_value, lv_color_hex(th_text));
    lv_style_set_text_font(&style_value, &lv_font_montserrat_14);
}

/* ── Helper: labelled slider row ──────────────────────────── */
static lv_obj_t *add_slider_row(lv_obj_t *parent, const char *text,
                                 int32_t min, int32_t max, int32_t val,
                                 lv_event_cb_t cb, int y_off)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_add_style(lbl, &style_label, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 14, y_off);

    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, 170, 8);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 14, y_off + 16);
    lv_obj_set_style_bg_color(sl, lv_color_hex(th_btn), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFF6644), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sl;
}

/* ── Helper: labelled switch row ──────────────────────────── */
static lv_obj_t *add_switch_row(lv_obj_t *parent, const char *text,
                                 bool on, lv_event_cb_t cb, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_add_style(lbl, &style_label, 0);
    lv_obj_set_pos(lbl, x, y);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 40, 20);
    lv_obj_set_pos(sw, x + 110, y - 2);
    lv_obj_set_style_bg_color(sw, lv_color_hex(th_btn), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFF6644), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ── Create ───────────────────────────────────────────────── */
void screen_settings_create(void)
{
    settings_load();
    init_styles();

    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* ════════════════════ LEFT COLUMN ═══════════════════════ */

    /* Brightness slider */
    add_slider_row(scr, LV_SYMBOL_IMAGE "  Brightness", 0, 255, 255,
                   brightness_cb, 22);

    /* Volume slider + percentage */
    lv_obj_t *vol_sl = add_slider_row(scr, LV_SYMBOL_VOLUME_MAX "  Volume",
                                       0, 255, g_volume, volume_cb, 54);
    (void)vol_sl;
    vol_pct_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(vol_pct_lbl, "%d%%", g_volume * 100 / 255);
    lv_obj_add_style(vol_pct_lbl, &style_value, 0);
    lv_obj_align(vol_pct_lbl, LV_ALIGN_TOP_LEFT, 192, 54);

    /* Boot Sound toggle */
    add_switch_row(scr, "Boot Sound", g_boot_sound_en, boot_sound_cb,
                   14, 86);

    /* 24h Clock toggle */
    add_switch_row(scr, "24h Clock", g_clock_24h, clock_24h_cb,
                   14, 106);

    /* Theme: Dark toggle */
    add_switch_row(scr, "Dark Theme", g_theme_dark, theme_cb,
                   14, 126);

    /* Clock: Flip toggle */
    add_switch_row(scr, "Flip Clock", g_clock_flip, clock_flip_cb,
                   14, 146);

    /* ════════════════════ CENTRE COLUMN ═════════════════════ */

    /* Device info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    lv_obj_t *l1 = lv_label_create(scr);
    lv_label_set_text_fmt(l1, "ESP32-S3  %d cores", chip.cores);
    lv_obj_add_style(l1, &style_label, 0);
    lv_obj_set_pos(l1, 240, 24);

    lv_obj_t *l2 = lv_label_create(scr);
    lv_label_set_text_fmt(l2, "Heap: %lu KB",
                          (unsigned long)(esp_get_free_heap_size() / 1024));
    lv_obj_add_style(l2, &style_label, 0);
    lv_obj_set_pos(l2, 240, 40);

    lv_obj_t *l3 = lv_label_create(scr);
    lv_label_set_text(l3, "FW: " FW_VERSION);
    lv_obj_add_style(l3, &style_value, 0);
    lv_obj_set_pos(l3, 240, 56);

    /* WiFi info */
    lv_obj_t *wifi_lbl = lv_label_create(scr);
    const char *cur_ssid = wifi_get_current_ssid();
    if (wifi_is_connected()) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            lv_label_set_text_fmt(wifi_lbl, LV_SYMBOL_WIFI " %s\n" IPSTR,
                                  cur_ssid, IP2STR(&ip_info.ip));
        } else {
            lv_label_set_text_fmt(wifi_lbl, LV_SYMBOL_WIFI " %s", cur_ssid);
        }
        lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0x00FF88), 0);
    } else {
        lv_label_set_text(wifi_lbl, LV_SYMBOL_WIFI " Not connected");
        lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0xFF4444), 0);
    }
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(wifi_lbl, 240, 78);

    /* ════════════════════ RIGHT COLUMN ════════════════════ */

    /* WiFi Setup button */
    lv_obj_t *wifi_btn = lv_btn_create(scr);
    lv_obj_set_size(wifi_btn, 120, 28);
    lv_obj_set_pos(wifi_btn, 520, 24);
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x0D47A1), 0);
    lv_obj_set_style_radius(wifi_btn, 8, 0);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(wifi_btn);
    lv_label_set_text(wl, LV_SYMBOL_WIFI " WiFi Setup");
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(wl);

    /* BLE button (small) */
    lv_obj_t *ble_btn = lv_btn_create(scr);
    lv_obj_set_size(ble_btn, 58, 28);
    lv_obj_set_pos(ble_btn, 520, 58);
    lv_obj_set_style_bg_color(ble_btn, lv_color_hex(0x1A3366), 0);
    lv_obj_set_style_radius(ble_btn, 8, 0);
    lv_obj_add_event_cb(ble_btn, ble_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btl = lv_label_create(ble_btn);
    lv_label_set_text(btl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(btl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(btl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btl);

    /* Reboot button */
    lv_obj_t *rbtn = lv_btn_create(scr);
    lv_obj_set_size(rbtn, 100, 28);
    lv_obj_set_pos(rbtn, 520, 92);
    lv_obj_set_style_bg_color(rbtn, lv_color_hex(0x882222), 0);
    lv_obj_set_style_radius(rbtn, 8, 0);
    lv_obj_add_event_cb(rbtn, reboot_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rbtn);
    lv_label_set_text(rl, LV_SYMBOL_POWER " Reboot");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(rl);

    /* ── Back button ──── */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 60, 26);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_bg_color(back, lv_color_hex(th_btn), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(th_text), 0);
    lv_obj_center(bl);

    lv_disp_load_scr(scr);
    ESP_LOGI(TAG, "Settings screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_settings_destroy(void)
{
    if (scr) {
        vol_pct_lbl = NULL;
        lv_obj_del(scr);
        scr = NULL;
    }
}
