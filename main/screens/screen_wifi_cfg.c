/**
 * @file screen_wifi_cfg.c
 * @brief On-device WiFi-provisioning status screen for 640×172 AMOLED.
 *
 * Shows instructions ("Connect to ESPro-Setup from your phone"),
 * real-time status, number of connected clients, and a Done/Cancel
 * button that stops provisioning and returns to settings.
 */

#include "screen_wifi_cfg.h"
#include "wifi_prov.h"
#include "app_manager.h"
#include "screen_settings.h"
#include "hw_config.h"
#include "wifi_time.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "wifi_cfg_scr";

static lv_obj_t *scr          = NULL;
static lv_obj_t *status_lbl   = NULL;
static lv_obj_t *clients_lbl  = NULL;
static lv_obj_t *hint_lbl     = NULL;
static lv_obj_t *done_btn     = NULL;

/* Animated dots for "Waiting…" */
static uint8_t s_dot_phase = 0;
static uint32_t s_last_tick = 0;

/* ── Callbacks ────────────────────────────────────────────── */
static void done_cb(lv_event_t *e)
{
    (void)e;
    wifi_prov_stop();          /* stops AP + HTTP + DNS, reconnects STA */
    wifi_reset_retry();        /* reset retry counter for fresh connection */
    app_manager_set_mode(MODE_SETTINGS);
}

/* ── Create ───────────────────────────────────────────────── */
void screen_wifi_cfg_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(th_bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ─── Left panel: instructions (x 0–340) ─── */

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi Setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6644), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 16, 8);

    /* Big WiFi icon area */
    lv_obj_t *icon_bg = lv_obj_create(scr);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 80, 80);
    lv_obj_set_pos(icon_bg, 16, 40);
    lv_obj_set_style_bg_color(icon_bg, lv_color_hex(0x1A2A3A), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 16, 0);

    lv_obj_t *icon = lv_label_create(icon_bg);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x00CCFF), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);

    /* Step instructions */
    lv_obj_t *s1 = lv_label_create(scr);
    lv_label_set_text(s1, "1. Connect phone WiFi to:");
    lv_obj_set_style_text_color(s1, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(s1, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s1, 110, 38);

    lv_obj_t *ap_name = lv_label_create(scr);
    lv_label_set_text(ap_name, "   ESPro-Setup");
    lv_obj_set_style_text_color(ap_name, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(ap_name, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ap_name, 110, 54);

    lv_obj_t *s2 = lv_label_create(scr);
    lv_label_set_text(s2, "2. Open browser:");
    lv_obj_set_style_text_color(s2, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(s2, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s2, 110, 76);

    lv_obj_t *ip_lbl = lv_label_create(scr);
    lv_label_set_text(ip_lbl, "   192.168.4.1");
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ip_lbl, 110, 92);

    lv_obj_t *s3 = lv_label_create(scr);
    lv_label_set_text(s3, "3. Select WiFi & save settings");
    lv_obj_set_style_text_color(s3, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(s3, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s3, 110, 114);

    /* ─── Right panel: status (x 380–630) ─── */

    lv_obj_t *st_card = lv_obj_create(scr);
    lv_obj_remove_style_all(st_card);
    lv_obj_set_size(st_card, 240, 130);
    lv_obj_set_pos(st_card, 390, 30);
    lv_obj_set_style_bg_color(st_card, lv_color_hex(th_card), 0);
    lv_obj_set_style_bg_opa(st_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st_card, 12, 0);
    lv_obj_clear_flag(st_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *st_title = lv_label_create(st_card);
    lv_label_set_text(st_title, "Status");
    lv_obj_set_style_text_color(st_title, lv_color_hex(th_label), 0);
    lv_obj_set_style_text_font(st_title, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(st_title, 12, 6);

    status_lbl = lv_label_create(st_card);
    lv_label_set_text(status_lbl, "Waiting for phone...");
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xFFCC00), 0);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(status_lbl, 12, 24);

    clients_lbl = lv_label_create(st_card);
    lv_label_set_text(clients_lbl, "Phones: 0");
    lv_obj_set_style_text_color(clients_lbl, lv_color_hex(th_text), 0);
    lv_obj_set_style_text_font(clients_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(clients_lbl, 12, 48);

    hint_lbl = lv_label_create(st_card);
    lv_label_set_text(hint_lbl, "Portal auto-opens on\nmost phones");
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(th_label), 0);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hint_lbl, 12, 68);

    /* Done / Cancel button */
    done_btn = lv_btn_create(st_card);
    lv_obj_set_size(done_btn, 100, 28);
    lv_obj_set_pos(done_btn, 125, 96);
    lv_obj_set_style_bg_color(done_btn, lv_color_hex(0x1B5E20), 0);
    lv_obj_set_style_radius(done_btn, 8, 0);
    lv_obj_add_event_cb(done_btn, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(done_btn);
    lv_label_set_text(bl, LV_SYMBOL_OK " Done");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* Start provisioning */
    wifi_prov_start();

    lv_disp_load_scr(scr);
    s_dot_phase = 0;
    s_last_tick = lv_tick_get();
    ESP_LOGI(TAG, "WiFi config screen created");
}

/* ── Destroy ──────────────────────────────────────────────── */
void screen_wifi_cfg_destroy(void)
{
    if (scr) {
        status_lbl  = NULL;
        clients_lbl = NULL;
        hint_lbl    = NULL;
        done_btn    = NULL;
        lv_obj_del(scr);
        scr = NULL;
    }
}

/* ── Update (called every LVGL tick) ──────────────────────── */
void screen_wifi_cfg_update(void)
{
    if (!scr || !status_lbl) return;

    /* Update at ~2 Hz */
    uint32_t now = lv_tick_get();
    if (now - s_last_tick < 500) return;
    s_last_tick = now;

    /* Connected AP clients count */
    int clients = wifi_prov_get_ap_clients();
    if (clients_lbl)
        lv_label_set_text_fmt(clients_lbl, "Phones: %d", clients);

    /* Status */
    wifi_prov_status_t st = wifi_prov_get_status();
    static const char dots[][4] = { ".", "..", "..." };
    s_dot_phase = (s_dot_phase + 1) % 3;

    switch (st) {
    case PROV_WAITING:
        if (clients > 0) {
            lv_label_set_text(status_lbl, "Phone connected!");
            lv_obj_set_style_text_color(status_lbl,
                lv_color_hex(0x00FF88), 0);
            if (hint_lbl)
                lv_label_set_text(hint_lbl,
                    "Open 192.168.4.1\nin the phone browser");
        } else {
            lv_label_set_text_fmt(status_lbl, "Waiting%s",
                                  dots[s_dot_phase]);
            lv_obj_set_style_text_color(status_lbl,
                lv_color_hex(0xFFCC00), 0);
        }
        break;

    case PROV_SAVED: {
        const char *ssid = wifi_prov_get_saved_ssid();
        lv_label_set_text_fmt(status_lbl, "Saved: %s", ssid);
        lv_obj_set_style_text_color(status_lbl,
            lv_color_hex(0x00FF88), 0);
        if (hint_lbl)
            lv_label_set_text(hint_lbl,
                "Press Done to connect\nwith new settings");
        break;
    }

    case PROV_CONNECTING:
        lv_label_set_text_fmt(status_lbl, "Connecting%s",
                              dots[s_dot_phase]);
        lv_obj_set_style_text_color(status_lbl,
            lv_color_hex(0xFFCC00), 0);
        break;

    case PROV_SUCCESS:
        lv_label_set_text(status_lbl, LV_SYMBOL_OK " Connected!");
        lv_obj_set_style_text_color(status_lbl,
            lv_color_hex(0x00FF88), 0);
        break;

    case PROV_FAILED:
        lv_label_set_text(status_lbl, LV_SYMBOL_CLOSE " Failed");
        lv_obj_set_style_text_color(status_lbl,
            lv_color_hex(0xFF4444), 0);
        if (hint_lbl)
            lv_label_set_text(hint_lbl,
                "Check password\nPress Done to retry");
        break;

    default:
        break;
    }
}
