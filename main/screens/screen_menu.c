/**
 * @file screen_menu.c
 * @brief App launcher menu with WiFi scan/connect, BLE scan, LED torch,
 *        and AI Assist (OpenAI).
 */

#include "screen_menu.h"
#include "app_manager.h"
#include "hw_config.h"
#include "screen_settings.h"
#include "wifi_time.h"
#include "sd_log.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_system.h"
#include "i2c_bsp.h"
#include <string.h>
#include <stdio.h>
#include "game_maze.h"
#include "music_player.h"
#include "macropad.h"

static const char *TAG = "menu";

static lv_obj_t *scr     = NULL;
static lv_obj_t *overlay = NULL;
static bool      led_on  = false;

/* ── WiFi scan state ──────────────────────────────────────── */
#define WIFI_SCAN_MAX  15
static wifi_ap_record_t s_wifi_aps[WIFI_SCAN_MAX];
static uint16_t         s_wifi_ap_count = 0;
static volatile bool    s_wifi_scanning = false;
static lv_obj_t        *wifi_list       = NULL;
static lv_obj_t        *wifi_status_lbl = NULL;
static char             s_selected_ssid[33];

/* ── WiFi password keyboard state ─────────────────────────── */
static lv_obj_t        *wifi_kb          = NULL;
static lv_obj_t        *wifi_ta          = NULL;
static lv_obj_t        *wifi_kb_overlay  = NULL;
static size_t           s_wifi_kb_ap_idx = 0;

/* ── BLE scan state ───────────────────────────────────────── */
#define BLE_SCAN_MAX   15
typedef struct {
    char    name[32];
    uint8_t addr[6];
    int8_t  rssi;
} ble_dev_t;
static ble_dev_t     s_ble_devs[BLE_SCAN_MAX];
static int           s_ble_dev_count = 0;
static volatile bool s_ble_scanning  = false;
static lv_obj_t     *ble_list        = NULL;
static lv_obj_t     *ble_status_lbl  = NULL;
static bool          s_ble_inited    = false;

/* ── AI Assist state ──────────────────────────────────────── */
static char          ai_response[512] = "";
static volatile bool ai_requesting    = false;
static volatile bool ai_response_ready = false;
static char          ai_prompt[128]   = "";
static lv_obj_t     *ai_response_lbl  = NULL;
static lv_obj_t     *ai_status_lbl    = NULL;

/* ── App entries ──────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *icon;
    uint32_t   color;
} menu_entry_t;

static const menu_entry_t apps[] = {
    { "LED",      LV_SYMBOL_CHARGE,    0x1B5E20 },
    { "AI",       LV_SYMBOL_EDIT,      0x6A1B9A },
    { "Game",     LV_SYMBOL_PLAY,      0x880E4F },
    { "Music",    LV_SYMBOL_AUDIO,     0x7B6800 },
    { "Buddy",    LV_SYMBOL_EYE_OPEN,  0x006064 },
    { "Macropad", LV_SYMBOL_KEYBOARD,  0x37474F },
    { "Settings", LV_SYMBOL_SETTINGS,  0x546E7A },
};
#define APP_COUNT (sizeof(apps) / sizeof(apps[0]))

static void close_overlay(void);
static void open_wifi_overlay(void);
static void open_ble_overlay(void);
static void open_led_overlay(void);
static void open_ai_overlay(void);

/* ── Close overlay ────────────────────────────────────────── */
static void back_btn_cb(lv_event_t *e) { (void)e; close_overlay(); }

static void close_overlay(void)
{
    if (overlay) { lv_obj_del(overlay); overlay = NULL; }
    wifi_list = NULL; wifi_status_lbl = NULL;
    wifi_kb = NULL; wifi_ta = NULL; wifi_kb_overlay = NULL;
    ble_list  = NULL; ble_status_lbl  = NULL;
    ai_response_lbl = NULL; ai_status_lbl = NULL;
}

static lv_obj_t *create_overlay_base(uint32_t bg_color)
{
    close_overlay();
    overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(overlay);
    lv_obj_set_size(back, 60, 30);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    return overlay;
}

/* ================================================================
 *  WiFi Scan / Connect
 * ================================================================ */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100, .scan_time.active.max = 300,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_OK) {
        s_wifi_ap_count = WIFI_SCAN_MAX;
        esp_wifi_scan_get_ap_records(&s_wifi_ap_count, s_wifi_aps);
        ESP_LOGI(TAG, "WiFi scan found %d APs", s_wifi_ap_count);
    } else {
        s_wifi_ap_count = 0;
    }
    s_wifi_scanning = false;
    vTaskDelete(NULL);
}

/* ── Connect to AP with given password ── */
static void wifi_do_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
    esp_wifi_disconnect();
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    if (wifi_status_lbl) {
        lv_label_set_text_fmt(wifi_status_lbl, "Connecting to %s...", ssid);
        lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0xFFCC00), 0);
    }
}

/* ── Close the password keyboard sub-overlay ── */
static void wifi_kb_close(void)
{
    if (wifi_kb_overlay) {
        lv_obj_del(wifi_kb_overlay);
        wifi_kb_overlay = NULL;
        wifi_kb = NULL;
        wifi_ta = NULL;
    }
}

/* ── Connect button on password overlay ── */
static void wifi_kb_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *pw = lv_textarea_get_text(wifi_ta);
    wifi_do_connect(s_selected_ssid, pw);
    wifi_kb_close();
}

/* ── Cancel button on password overlay ── */
static void wifi_kb_cancel_cb(lv_event_t *e)
{
    (void)e;
    wifi_kb_close();
}

/* ── Toggle password visibility ── */
static void wifi_eye_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (!wifi_ta) return;
    bool cur = lv_textarea_get_password_mode(wifi_ta);
    lv_textarea_set_password_mode(wifi_ta, !cur);
}

/* ── Show password entry keyboard overlay ── */
static void wifi_show_password_entry(void)
{
    if (!overlay) return;

    /* Create sub-overlay on top of the WiFi overlay */
    wifi_kb_overlay = lv_obj_create(overlay);
    lv_obj_remove_style_all(wifi_kb_overlay);
    lv_obj_set_size(wifi_kb_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(wifi_kb_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_kb_overlay, lv_color_hex(th_bg), 0);
    lv_obj_set_style_bg_opa(wifi_kb_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wifi_kb_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* SSID label */
    lv_obj_t *ssid_lbl = lv_label_create(wifi_kb_overlay);
    lv_label_set_text_fmt(ssid_lbl, LV_SYMBOL_WIFI " %s", s_selected_ssid);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

    /* Cancel button */
    lv_obj_t *cancel_btn = lv_btn_create(wifi_kb_overlay);
    lv_obj_set_size(cancel_btn, 60, 24);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, -80, 2);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, wifi_kb_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);

    /* Connect button */
    lv_obj_t *conn_btn = lv_btn_create(wifi_kb_overlay);
    lv_obj_set_size(conn_btn, 70, 24);
    lv_obj_align(conn_btn, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_obj_set_style_bg_color(conn_btn, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(conn_btn, 6, 0);
    lv_obj_add_event_cb(conn_btn, wifi_kb_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(conn_btn);
    lv_label_set_text(cnl, LV_SYMBOL_OK " Join");
    lv_obj_set_style_text_font(cnl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cnl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cnl);

    /* Password textarea */
    wifi_ta = lv_textarea_create(wifi_kb_overlay);
    lv_obj_set_size(wifi_ta, 300, 28);
    lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 28);
    lv_textarea_set_placeholder_text(wifi_ta, "Enter password...");
    lv_textarea_set_password_mode(wifi_ta, true);
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_max_length(wifi_ta, 63);
    lv_obj_set_style_text_font(wifi_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(wifi_ta, lv_color_hex(0x1A2A3A), 0);
    lv_obj_set_style_text_color(wifi_ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(wifi_ta, lv_color_hex(0x4488FF), 0);
    lv_obj_set_style_border_width(wifi_ta, 1, 0);
    lv_obj_set_style_radius(wifi_ta, 6, 0);

    /* Show/hide password toggle */
    lv_obj_t *eye_btn = lv_btn_create(wifi_kb_overlay);
    lv_obj_set_size(eye_btn, 28, 28);
    lv_obj_align_to(eye_btn, wifi_ta, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    lv_obj_set_style_bg_color(eye_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(eye_btn, 6, 0);
    lv_obj_add_event_cb(eye_btn, wifi_eye_toggle_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_lbl = lv_label_create(eye_btn);
    lv_label_set_text(eye_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(eye_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(eye_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(eye_lbl);

    /* LVGL keyboard — fill bottom area */
    wifi_kb = lv_keyboard_create(wifi_kb_overlay);
    lv_obj_set_size(wifi_kb, 620, 112);
    lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(wifi_kb, wifi_ta);
    lv_obj_set_style_text_font(wifi_kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(0x2A2A4A), LV_PART_ITEMS);
    lv_obj_set_style_text_color(wifi_kb, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(0x3355AA),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
}

static void wifi_ap_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_wifi_ap_count) return;

    strncpy(s_selected_ssid, (char *)s_wifi_aps[idx].ssid,
            sizeof(s_selected_ssid) - 1);
    s_selected_ssid[sizeof(s_selected_ssid) - 1] = '\0';
    s_wifi_kb_ap_idx = idx;

    /* Open network → connect directly. Secured → show password entry */
    if (s_wifi_aps[idx].authmode == WIFI_AUTH_OPEN) {
        wifi_do_connect(s_selected_ssid, NULL);
    } else if (strcmp(s_selected_ssid, WIFI_SSID) == 0) {
        /* Known network from hw_config.h → auto-fill password */
        wifi_do_connect(s_selected_ssid, WIFI_PASS);
    } else {
        wifi_show_password_entry();
    }
}

static void wifi_populate_list(void)
{
    if (!wifi_list || !overlay) return;
    lv_obj_clean(wifi_list);

    for (uint16_t i = 0; i < s_wifi_ap_count; i++) {
        lv_obj_t *btn = lv_btn_create(wifi_list);
        lv_obj_set_size(btn, 280, 28);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2A3A), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        lv_obj_add_event_cb(btn, wifi_ap_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        char txt[52];
        const char *lock = (s_wifi_aps[i].authmode != WIFI_AUTH_OPEN)
                           ? LV_SYMBOL_EYE_CLOSE : "";
        snprintf(txt, sizeof(txt), "%s %s  %ddBm",
                 lock, (char *)s_wifi_aps[i].ssid, s_wifi_aps[i].rssi);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }
    if (wifi_status_lbl) {
        lv_label_set_text_fmt(wifi_status_lbl, "%d networks found",
                              s_wifi_ap_count);
        lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0x00FF88), 0);
    }
}

static void wifi_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_scanning) return;
    s_wifi_scanning = true;
    if (wifi_status_lbl) {
        lv_label_set_text(wifi_status_lbl, "Scanning...");
        lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0xFFCC00), 0);
    }
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 3, NULL);
}

static void open_wifi_overlay(void)
{
    lv_obj_t *p = create_overlay_base(th_bg);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    bool connected = wifi_is_connected();
    lv_obj_t *conn = lv_label_create(p);
    char ctxt[64];
    if (connected) {
        char ip[20] = "";
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(nif, &ipi) == ESP_OK)
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
        }
        snprintf(ctxt, sizeof(ctxt), "%s  %s", WIFI_SSID, ip);
    } else {
        snprintf(ctxt, sizeof(ctxt), "Not connected");
    }
    lv_label_set_text(conn, ctxt);
    lv_obj_set_style_text_color(conn,
        connected ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_text_font(conn, &lv_font_montserrat_12, 0);
    lv_obj_align(conn, LV_ALIGN_TOP_LEFT, 80, 10);

    lv_obj_t *sb = lv_btn_create(p);
    lv_obj_set_size(sb, 70, 28);
    lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x1A5A3A), 0);
    lv_obj_set_style_radius(sb, 8, 0);
    lv_obj_add_event_cb(sb, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(sl);

    wifi_status_lbl = lv_label_create(p);
    lv_label_set_text(wifi_status_lbl, "Tap Scan to find networks");
    lv_obj_set_style_text_font(wifi_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(wifi_status_lbl, LV_ALIGN_TOP_MID, 0, 34);

    wifi_list = lv_obj_create(p);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_size(wifi_list, 300, 110);
    lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wifi_list, 4, 0);
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);
}

/* ================================================================
 *  BLE Scan
 * ================================================================ */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (s_ble_dev_count >= BLE_SCAN_MAX) return 0;

        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                          event->disc.length_data);
        char name[32] = "";
        if (rc == 0 && fields.name != NULL && fields.name_len > 0) {
            int len = fields.name_len < 31 ? fields.name_len : 31;
            memcpy(name, fields.name, len);
            name[len] = '\0';
        }
        for (int i = 0; i < s_ble_dev_count; i++) {
            if (memcmp(s_ble_devs[i].addr, event->disc.addr.val, 6) == 0)
                return 0;
        }
        ble_dev_t *d = &s_ble_devs[s_ble_dev_count];
        memcpy(d->addr, event->disc.addr.val, 6);
        strncpy(d->name, name[0] ? name : "(unknown)", sizeof(d->name) - 1);
        d->rssi = event->disc.rssi;
        s_ble_dev_count++;
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_ble_scanning = false;
    }
    return 0;
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init_once(void)
{
    if (s_ble_inited) return;
    /* If macropad already brought up NimBLE + HID, reuse it */
    if (macropad_ble_inited()) {
        s_ble_inited = true;
        return;
    }
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(ret));
        return;
    }
    nimble_port_freertos_init(ble_host_task);
    vTaskDelay(pdMS_TO_TICKS(500));
    s_ble_inited = true;
}

static void ble_start_scan(void)
{
    s_ble_dev_count = 0;
    s_ble_scanning  = true;
    struct ble_gap_disc_params dp = {
        .itvl = 0, .window = 0, .filter_policy = 0,
        .limited = 0, .passive = 0, .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &dp,
                          ble_gap_event_cb, NULL);
    if (rc != 0) {
        s_ble_scanning = false;
    }
}

static void ble_populate_list(void)
{
    if (!ble_list || !overlay) return;
    lv_obj_clean(ble_list);

    for (int i = 0; i < s_ble_dev_count; i++) {
        lv_obj_t *row = lv_obj_create(ble_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 280, 26);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2A3A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char txt[52];
        snprintf(txt, sizeof(txt), "%s  %ddBm",
                 s_ble_devs[i].name, s_ble_devs[i].rssi);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }
    if (ble_status_lbl) {
        lv_label_set_text_fmt(ble_status_lbl, "%d devices found",
                              s_ble_dev_count);
        lv_obj_set_style_text_color(ble_status_lbl, lv_color_hex(0x00FF88), 0);
    }
}

static void ble_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_ble_scanning) return;
    ble_init_once();
    if (!s_ble_inited) return;
    if (ble_status_lbl) {
        lv_label_set_text(ble_status_lbl, "Scanning...");
        lv_obj_set_style_text_color(ble_status_lbl, lv_color_hex(0xFFCC00), 0);
    }
    ble_start_scan();
}

static void open_ble_overlay(void)
{
    lv_obj_t *p = create_overlay_base(th_bg);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, LV_SYMBOL_BLUETOOTH "  BLE");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *nm = lv_label_create(p);
    lv_label_set_text(nm, "Device: SS_Smallstart");
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 80, 10);

    lv_obj_t *sb = lv_btn_create(p);
    lv_obj_set_size(sb, 70, 28);
    lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x1A3366), 0);
    lv_obj_set_style_radius(sb, 8, 0);
    lv_obj_add_event_cb(sb, ble_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(sb);
    lv_label_set_text(sl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(sl);

    ble_status_lbl = lv_label_create(p);
    lv_label_set_text(ble_status_lbl, "Tap Scan to find devices");
    lv_obj_set_style_text_font(ble_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(ble_status_lbl, LV_ALIGN_TOP_MID, 0, 34);

    ble_list = lv_obj_create(p);
    lv_obj_remove_style_all(ble_list);
    lv_obj_set_size(ble_list, 300, 110);
    lv_obj_align(ble_list, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(ble_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ble_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ble_list, 4, 0);
    lv_obj_add_flag(ble_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ble_list, LV_DIR_VER);
}

/* ================================================================
 *  AI Assist — Free HuggingFace Inference API
 *  Based on: github.com/derdacavga/Esp32-Ai-Voice-Assistant
 *  Uses google/flan-t5-base (same LLM as the referenced project)
 * ================================================================ */

/* ── HuggingFace AI request task ── */
static void hf_request_task(void *arg)
{
    (void)arg;
    char *buf = malloc(4096);
    if (!buf) {
        strncpy(ai_response, "Memory error", sizeof(ai_response) - 1);
        ai_response_ready = true;
        ai_requesting = false;
        vTaskDelete(NULL);
        return;
    }

    /* Build URL: https://router.huggingface.co/models/{model} */
    char url[256];
    snprintf(url, sizeof(url),
             "https://router.huggingface.co/hf-inference/models/%s", HF_MODEL);

    /* Build JSON body */
    char body[512];
    snprintf(body, sizeof(body),
        "{\"inputs\":\"%s\",\"parameters\":"
        "{\"max_new_tokens\":100,\"temperature\":0.7,\"do_sample\":true}}",
        ai_prompt);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", HF_API_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        snprintf(ai_response, sizeof(ai_response), "Connection error");
        goto done;
    }

    esp_http_client_write(client, body, strlen(body));
    esp_http_client_fetch_headers(client);

    int total = 0, n;
    while ((n = esp_http_client_read(client, buf + total, 4095 - total)) > 0) {
        total += n;
        if (total >= 4095) break;
    }
    buf[total] = '\0';

    /* Parse response: [{"generated_text":"..."}] or {"error":"..."} */
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        if (cJSON_IsArray(root)) {
            cJSON *first = cJSON_GetArrayItem(root, 0);
            if (first) {
                cJSON *gt = cJSON_GetObjectItem(first, "generated_text");
                if (gt && cJSON_IsString(gt)) {
                    strncpy(ai_response, gt->valuestring,
                            sizeof(ai_response) - 1);
                    ai_response[sizeof(ai_response) - 1] = '\0';
                }
            }
        } else if (cJSON_IsObject(root)) {
            cJSON *errj = cJSON_GetObjectItem(root, "error");
            if (errj && cJSON_IsString(errj)) {
                snprintf(ai_response, sizeof(ai_response),
                         "HF error: %.200s", errj->valuestring);
            }
        }
        cJSON_Delete(root);
    } else {
        snprintf(ai_response, sizeof(ai_response), "Parse error");
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    ai_response_ready = true;
    ai_requesting = false;
    vTaskDelete(NULL);
}

static void ai_prompt_btn_cb(lv_event_t *e)
{
    if (ai_requesting) return;
    const char *prompt = (const char *)lv_event_get_user_data(e);
    strncpy(ai_prompt, prompt, sizeof(ai_prompt) - 1);
    ai_prompt[sizeof(ai_prompt) - 1] = '\0';
    ai_response[0] = '\0';
    ai_requesting = true;
    ai_response_ready = false;

    if (ai_status_lbl) {
        lv_label_set_text(ai_status_lbl, "Thinking...");
        lv_obj_set_style_text_color(ai_status_lbl, lv_color_hex(0xFFCC00), 0);
    }
    if (ai_response_lbl)
        lv_label_set_text(ai_response_lbl, "");

    xTaskCreate(hf_request_task, "ai_req", 8192, NULL, 3, NULL);
}

static const char *ai_prompts[] = {
    "Give a brief weather tip for Bengaluru India in under 30 words",
    "Give me a short motivational quote in under 20 words",
    "Tell me an interesting fun fact in under 30 words",
    "Tell me a short clean joke in under 30 words",
};

static void open_ai_overlay(void)
{
    lv_obj_t *p = create_overlay_base(th_bg);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, LV_SYMBOL_EDIT "  AI Assist (HuggingFace)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* Status label */
    ai_status_lbl = lv_label_create(p);
    lv_label_set_text(ai_status_lbl, "Tap a button to ask AI");
    lv_obj_set_style_text_font(ai_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ai_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(ai_status_lbl, LV_ALIGN_TOP_MID, 0, 26);

    /* Preset prompt buttons */
    static const char *btn_labels[] = {"Weather", "Motivate", "Fact", "Joke"};
    lv_obj_t *btn_row = lv_obj_create(p);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 500, 28);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(btn_row);
        lv_obj_set_size(btn, 100, 24);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x6A1B9A), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, ai_prompt_btn_cb, LV_EVENT_CLICKED,
                            (void *)ai_prompts[i]);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, btn_labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    /* Response area (scrollable) */
    lv_obj_t *resp_box = lv_obj_create(p);
    lv_obj_remove_style_all(resp_box);
    lv_obj_set_size(resp_box, 580, 90);
    lv_obj_align(resp_box, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(resp_box, lv_color_hex(0x111122), 0);
    lv_obj_set_style_bg_opa(resp_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(resp_box, 8, 0);
    lv_obj_set_style_pad_all(resp_box, 6, 0);
    lv_obj_add_flag(resp_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(resp_box, LV_DIR_VER);

    ai_response_lbl = lv_label_create(resp_box);
    lv_label_set_long_mode(ai_response_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ai_response_lbl, 568);
    lv_label_set_text(ai_response_lbl,
        ai_response[0] ? ai_response : "Response will appear here...");
    lv_obj_set_style_text_font(ai_response_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ai_response_lbl, lv_color_hex(0xDDDDDD), 0);

    ESP_LOGI(TAG, "AI overlay opened");
}

/* ================================================================
 *  LED Torch Overlay
 * ================================================================ */
static lv_obj_t *led_btn     = NULL;
static lv_obj_t *led_btn_lbl = NULL;

static void led_toggle_cb(lv_event_t *e)
{
    (void)e;
    led_on = !led_on;
    if (led_on) {
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(led_btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_text_color(led_btn_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(led_btn_lbl, "ON");
    } else {
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_color(led_btn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(led_btn_lbl, lv_color_hex(0x000000), 0);
        lv_label_set_text(led_btn_lbl, "OFF");
    }
}

static void led_back_cb(lv_event_t *e)
{
    (void)e;
    led_on = false; led_btn = NULL; led_btn_lbl = NULL;
    close_overlay();
}

static void open_led_overlay(void)
{
    close_overlay();
    overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(overlay);
    lv_obj_set_size(back, 60, 30);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, led_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    led_btn = lv_btn_create(overlay);
    lv_obj_set_size(led_btn, 120, 120);
    lv_obj_align(led_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(led_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(led_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(led_btn, 20, 0);
    lv_obj_set_style_shadow_color(led_btn, lv_color_hex(0x888888), 0);
    lv_obj_set_style_shadow_opa(led_btn, LV_OPA_50, 0);
    lv_obj_add_event_cb(led_btn, led_toggle_cb, LV_EVENT_CLICKED, NULL);

    led_btn_lbl = lv_label_create(led_btn);
    lv_label_set_text(led_btn_lbl, "OFF");
    lv_obj_set_style_text_font(led_btn_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(led_btn_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(led_btn_lbl);
    led_on = false;
}

/* ── Card tap callback ────────────────────────────────────── */
/* Pending overlay request from settings screen */
static volatile int s_pending_overlay = -1;   /* -1=none, 0=BLE, 1=WiFi */

void screen_menu_request_wifi(void) { s_pending_overlay = 1; }
void screen_menu_request_ble(void)  { s_pending_overlay = 0; }

static void card_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "App tapped: %s (#%d)", apps[idx].name, (int)idx);
    switch (idx) {
        case 0: open_led_overlay();     break;
        case 1: open_ai_overlay();      break;
        case 2: game_maze_open(scr);    break;
        case 3: music_player_open(scr); break;
        case 4: app_manager_set_mode(MODE_TAMAFI); break;
        case 5: macropad_open(scr);     break;
        case 6: app_manager_set_mode(MODE_SETTINGS); break;
        default: break;
    }
}

/* ── Styles ───────────────────────────────────────────────── */
static lv_style_t style_bg, style_card, style_icon, style_name;
static lv_style_t style_dot_active, style_dot_inactive;
static lv_obj_t *mode_dot[2];

static void init_styles(void)
{
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_hex(th_bg));
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

    lv_style_init(&style_card);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_pad_all(&style_card, 6);
    lv_style_set_width(&style_card, 80);
    lv_style_set_height(&style_card, 100);

    lv_style_init(&style_icon);
    lv_style_set_text_color(&style_icon, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_icon, &lv_font_montserrat_28);

    lv_style_init(&style_name);
    lv_style_set_text_color(&style_name, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&style_name, &lv_font_montserrat_12);

    lv_style_init(&style_dot_active);
    lv_style_set_bg_color(&style_dot_active, lv_color_hex(0xFF8844));
    lv_style_set_bg_opa(&style_dot_active, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_active, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_active, 8);
    lv_style_set_height(&style_dot_active, 8);

    lv_style_init(&style_dot_inactive);
    lv_style_set_bg_color(&style_dot_inactive, lv_color_hex(th_btn));
    lv_style_set_bg_opa(&style_dot_inactive, LV_OPA_COVER);
    lv_style_set_radius(&style_dot_inactive, LV_RADIUS_CIRCLE);
    lv_style_set_width(&style_dot_inactive, 6);
    lv_style_set_height(&style_dot_inactive, 6);
}

/* ── Periodic update (called from app_manager) ────────────── */
void screen_menu_update(void)
{
    if (!scr) return;

    /* Auto-open overlay requested by settings screen */
    if (s_pending_overlay >= 0) {
        int p = s_pending_overlay;
        s_pending_overlay = -1;
        if (p == 0) open_ble_overlay();
        else if (p == 1) open_wifi_overlay();
    }

    /* WiFi scan results */
    if (!s_wifi_scanning && wifi_list && s_wifi_ap_count > 0) {
        static uint16_t prev_wifi_count = 0xFFFF;
        if (s_wifi_ap_count != prev_wifi_count) {
            prev_wifi_count = s_wifi_ap_count;
            wifi_populate_list();
        }
    }

    /* BLE scan results */
    if (!s_ble_scanning && ble_list && s_ble_dev_count > 0) {
        static int prev_ble_count = -1;
        if (s_ble_dev_count != prev_ble_count) {
            prev_ble_count = s_ble_dev_count;
            ble_populate_list();
        }
    }

    /* Maze game tick */
    if (game_maze_is_active()) {
        game_maze_update();
    }

    /* Music player tick */
    if (music_player_is_active()) {
        music_player_update();
    }

    /* Macropad — nothing to tick, but close check */

    /* AI response ready */
    if (ai_response_ready && ai_response_lbl) {
        ai_response_ready = false;
        lv_label_set_text(ai_response_lbl, ai_response);
        if (ai_status_lbl) {
            lv_label_set_text(ai_status_lbl, "Done");
            lv_obj_set_style_text_color(ai_status_lbl,
                                        lv_color_hex(0x00FF88), 0);
        }
    }
}

/* ── Create ───────────────────────────────────────────────── */
void screen_menu_create(void)
{
    init_styles();
    scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 620, 110);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_CENTER);

    for (size_t i = 0; i < APP_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(apps[i].color), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, apps[i].icon);
        lv_obj_add_style(icon, &style_icon, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, apps[i].name);
        lv_obj_add_style(name, &style_name, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    /* Page dots (2) */
    lv_obj_t *dot_row = lv_obj_create(scr);
    lv_obj_remove_style_all(dot_row);
    lv_obj_set_size(dot_row, 40, 12);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 8, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        mode_dot[i] = lv_obj_create(dot_row);
        lv_obj_remove_style_all(mode_dot[i]);
        if (i == 1)
            lv_obj_add_style(mode_dot[i], &style_dot_active, 0);
        else
            lv_obj_add_style(mode_dot[i], &style_dot_inactive, 0);
    }

    lv_disp_load_scr(scr);
    ESP_LOGI(TAG, "Menu screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_menu_destroy(void)
{
    if (scr) {
        game_maze_close();
        music_player_close();
        macropad_close();
        overlay = NULL; led_btn = NULL; led_btn_lbl = NULL; led_on = false;
        wifi_list = NULL; wifi_status_lbl = NULL;
        ble_list = NULL; ble_status_lbl = NULL;
        ai_response_lbl = NULL; ai_status_lbl = NULL;
        lv_obj_del(scr);
        scr = NULL;
    }
}
