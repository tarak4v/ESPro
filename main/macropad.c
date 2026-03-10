/**
 * @file macropad.c
 * @brief BLE HID + WiFi UDP macropad — touch-based media control.
 *
 * Two transport modes (toggled in the overlay UI):
 *  - BLE HID: Advertises as a Consumer Control device ("ESPro Macropad").
 *             PC pairs directly — no dongle needed.
 *  - WiFi UDP: Sends key-name strings to 255.255.255.255:13579.
 *             A small Python listener on the PC executes the mapped action.
 */

#include "macropad.h"
#include "hw_config.h"
#include "wifi_time.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"

/* Forward-declare NimBLE store init (defined in ble_store_config.c, no public header) */
void ble_store_config_init(void);
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "macropad";

/* ═══════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════ */
#define MP_DEVICE_NAME   "ESPro Macropad"
#define MP_UDP_PORT      13579
#define MP_UDP_RX_PORT   13580          /* PC → ESP32 state feedback  */
#define HID_RPT_ID_KBD   1          /* Keyboard report ID          */
#define HID_KBD_RPT_LEN  8          /* 1 mod + 1 rsvd + 6 keys     */
#define HID_RPT_ID_CC    3          /* Consumer Control report ID  */
#define HID_CC_RPT_LEN   2          /* Consumer Control report len */

/* HID keyboard modifier bits */
#define HID_MOD_LCTRL    0x01
#define HID_MOD_LSHIFT   0x02
/* HID keycodes */
#define HID_KEY_D        0x07
#define HID_KEY_E        0x08
#define HID_KEY_H        0x0B
#define HID_KEY_M        0x10
#define HID_KEY_O        0x12
#define HID_KEY_W        0x1A

/* ═══════════════════════════════════════════════════════════
 *  Transport mode
 * ═══════════════════════════════════════════════════════════ */
typedef enum { MP_MODE_BLE = 0, MP_MODE_WIFI = 1 } mp_mode_t;
static mp_mode_t s_mode = MP_MODE_BLE;

/* ═══════════════════════════════════════════════════════════
 *  App mode (different keyboard shortcuts per app)
 * ═══════════════════════════════════════════════════════════ */
typedef enum { MP_APP_GMEET = 0, MP_APP_TEAMS = 1 } mp_app_t;
static mp_app_t s_app = MP_APP_GMEET;

/* ═══════════════════════════════════════════════════════════
 *  Key definitions
 * ═══════════════════════════════════════════════════════════ */
typedef enum {
    MP_KEY_VOL_UP = 0,
    MP_KEY_VOL_DOWN,
    MP_KEY_VIDEO_TOGGLE,
    MP_KEY_MIC_TOGGLE,
    MP_KEY_END_MEETING,
    MP_KEY_COUNT
} mp_key_t;

static const char *s_udp_names[MP_KEY_COUNT] = {
    "vol_up", "vol_down", "video_toggle", "mic_toggle", "end_meeting",
};

/* ═══════════════════════════════════════════════════════════
 *  BLE HID — Combined Report Map (Keyboard + Consumer Control)
 * ═══════════════════════════════════════════════════════════ */
static const uint8_t s_cc_report_map[] = {
    /* ---- Keyboard (Report ID 1) ---- */
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Min (Left Control)
    0x29, 0xE7,        //   Usage Max (Right GUI)
    0x15, 0x00,        //   Logical Min (0)
    0x25, 0x01,        //   Logical Max (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) — Modifiers
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const) — Reserved byte
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Min (Num Lock)
    0x29, 0x05,        //   Usage Max (Kana)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x05,        //   Report Count (5)
    0x91, 0x02,        //   Output (Data,Var,Abs) — LED states
    0x75, 0x03,        //   Report Size (3)
    0x95, 0x01,        //   Report Count (1)
    0x91, 0x01,        //   Output (Const) — LED padding
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Min (0)
    0x29, 0x65,        //   Usage Max (101)
    0x15, 0x00,        //   Logical Min (0)
    0x25, 0x65,        //   Logical Max (101)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x06,        //   Report Count (6)
    0x81, 0x00,        //   Input (Data,Ary,Abs) — Key array
    0xC0,              // End Collection

    /* ---- Consumer Control (Report ID 3) ---- */
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x09, 0x02,        //   Usage (Numeric Key Pad)
    0xA1, 0x02,        //   Collection (Logical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x0A,        //     Usage Maximum (Button 10)
    0x15, 0x01,        //     Logical Minimum (1)
    0x25, 0x0A,        //     Logical Maximum (10)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x00,        //     Input (Data,Ary,Abs)
    0xC0,              //   End Collection
    0x05, 0x0C,        //   Usage Page (Consumer)
    0x09, 0x86,        //   Usage (Channel)
    0x15, 0xFF,        //   Logical Minimum (-1)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x02,        //   Report Size (2)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x46,        //   Input (Data,Var,Rel,Null)
    0x09, 0xE9,        //   Usage (Volume Increment)
    0x09, 0xEA,        //   Usage (Volume Decrement)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x02,        //   Report Count (2)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x09, 0xE2,        //   Usage (Mute)
    0x09, 0x30,        //   Usage (Power)
    0x09, 0x83,        //   Usage (Recall Last)
    0x09, 0x81,        //   Usage (Assign Selection)
    0x09, 0xB0,        //   Usage (Play)
    0x09, 0xB1,        //   Usage (Pause)
    0x09, 0xB2,        //   Usage (Record)
    0x09, 0xB3,        //   Usage (Fast Forward)
    0x09, 0xB4,        //   Usage (Rewind)
    0x09, 0xB5,        //   Usage (Scan Next Track)
    0x09, 0xB6,        //   Usage (Scan Previous Track)
    0x09, 0xB7,        //   Usage (Stop)
    0x15, 0x01,        //   Logical Minimum (1)
    0x25, 0x0C,        //   Logical Maximum (12)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data,Ary,Abs)
    0x09, 0x80,        //   Usage (Selection)
    0xA1, 0x02,        //   Collection (Logical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x01,        //     Logical Minimum (1)
    0x25, 0x03,        //     Logical Maximum (3)
    0x75, 0x02,        //     Report Size (2)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x00,        //     Input (Data,Ary,Abs)
    0xC0,              //   End Collection
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0xC0,              // End Collection
};

/* ═══════════════════════════════════════════════════════════
 *  BLE HID state
 * ═══════════════════════════════════════════════════════════ */
static esp_hidd_dev_t  *s_hid_dev     = NULL;
static volatile bool    s_ble_inited  = false;
static volatile bool    s_ble_conn    = false;

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_cc_report_map, .len = sizeof(s_cc_report_map) },
};

static esp_hid_device_config_t s_hid_cfg = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = MP_DEVICE_NAME,
    .manufacturer_name  = "ESPro",
    .serial_number      = "1234567890",
    .report_maps        = s_report_maps,
    .report_maps_len    = 1,
};

/* Advertising field storage that persists across restarts */
static struct ble_hs_adv_fields s_adv_fields;
static ble_uuid16_t             s_hid_uuid;

/* ═══════════════════════════════════════════════════════════
 *  Key-send task (sends from FreeRTOS context, not LVGL)
 * ═══════════════════════════════════════════════════════════ */
static QueueHandle_t s_key_queue  = NULL;
static TaskHandle_t  s_send_task  = NULL;
static TaskHandle_t  s_rx_task    = NULL;

/* Thread-safe status update for LVGL label */
static volatile bool s_status_dirty = false;
static char          s_status_buf[48];

static void set_status(const char *txt)
{
    snprintf(s_status_buf, sizeof(s_status_buf), "%s", txt);
    s_status_dirty = true;
}

/* ═══════════════════════════════════════════════════════════
 *  BLE advertising helpers
 * ═══════════════════════════════════════════════════════════ */
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

static void adv_fields_setup(void)
{
    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.tx_pwr_lvl_is_present = 1;
    s_adv_fields.tx_pwr_lvl           = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    s_adv_fields.appearance           = 0x03C0; /* HID Generic */
    s_adv_fields.appearance_is_present = 1;
    s_adv_fields.name                 = (uint8_t *)MP_DEVICE_NAME;
    s_adv_fields.name_len             = strlen(MP_DEVICE_NAME);
    s_adv_fields.name_is_complete     = 1;

    s_hid_uuid = (ble_uuid16_t)BLE_UUID16_INIT(0x1812);
    s_adv_fields.uuids16              = &s_hid_uuid;
    s_adv_fields.num_uuids16          = 1;
    s_adv_fields.uuids16_is_complete  = 1;
}

static esp_err_t start_ble_adv(void)
{
    int rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields: %d", rc);
        return ESP_FAIL;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, 180000,
                           &params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        return ESP_OK;              /* already advertising */
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BLE advertising started");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════
 *  GAP event callback (passed to ble_gap_adv_start)
 * ═══════════════════════════════════════════════════════════ */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_conn = true;
            set_status(LV_SYMBOL_BLUETOOTH " Connected");
            ESP_LOGI(TAG, "BLE connected");
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_ble_conn = false;
        set_status(LV_SYMBOL_BLUETOOTH " Disconnected");
        ESP_LOGI(TAG, "BLE disconnected, restarting adv");
        start_ble_adv();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_ble_conn) {
            start_ble_adv();
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption changed, status=%d", event->enc_change.status);
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action  = BLE_SM_IOACT_DISP;
            pkey.passkey = 123456;
            ESP_LOGI(TAG, "Passkey: %" PRIu32, pkey.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action        = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  HIDD event callback (fired by esp_hid layer)
 * ═══════════════════════════════════════════════════════════ */
static void hidd_event_cb(void *handler_args, esp_event_base_t base,
                           int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HIDD started — begin advertising");
        set_status(LV_SYMBOL_BLUETOOTH " Advertising...");
        start_ble_adv();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_ble_conn = true;
        set_status(LV_SYMBOL_BLUETOOTH " Connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_ble_conn = false;
        set_status(LV_SYMBOL_BLUETOOTH " Disconnected");
        start_ble_adv();
        break;
    case ESP_HIDD_STOP_EVENT:
        set_status("BLE stopped");
        break;
    default:
        break;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  NimBLE host task
 * ═══════════════════════════════════════════════════════════ */
static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ═══════════════════════════════════════════════════════════
 *  BLE send — Consumer Control report
 * ═══════════════════════════════════════════════════════════ */
static void send_cc_report(mp_key_t key)
{
    uint8_t buf[HID_CC_RPT_LEN] = {0, 0};

    switch (key) {
    case MP_KEY_VOL_UP:      buf[0] = 0x40;  break;
    case MP_KEY_VOL_DOWN:    buf[0] = 0x80;  break;
    default: return;
    }

    esp_hidd_dev_input_set(s_hid_dev, 0, HID_RPT_ID_CC, buf, HID_CC_RPT_LEN);
}

/* Send a keyboard shortcut (modifier + key), then release */
static void send_kbd_shortcut(uint8_t modifier, uint8_t keycode)
{
    uint8_t buf[HID_KBD_RPT_LEN] = {0};
    buf[0] = modifier;
    buf[2] = keycode;
    esp_hidd_dev_input_set(s_hid_dev, 0, HID_RPT_ID_KBD, buf, HID_KBD_RPT_LEN);
    vTaskDelay(pdMS_TO_TICKS(30));
    /* Release all keys */
    memset(buf, 0, sizeof(buf));
    esp_hidd_dev_input_set(s_hid_dev, 0, HID_RPT_ID_KBD, buf, HID_KBD_RPT_LEN);
}

/* ═══════════════════════════════════════════════════════════
 *  WiFi send — UDP broadcast
 * ═══════════════════════════════════════════════════════════ */
static void send_udp_key(const char *name)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(MP_UDP_PORT),
    };
    inet_pton(AF_INET, "255.255.255.255", &dest.sin_addr);

    sendto(sock, name, strlen(name), 0,
           (struct sockaddr *)&dest, sizeof(dest));
    close(sock);
}

/* ═══════════════════════════════════════════════════════════
 *  Background key-send task
 * ═══════════════════════════════════════════════════════════ */
static void key_send_task(void *arg)
{
    (void)arg;
    mp_key_t key;
    while (1) {
        if (xQueueReceive(s_key_queue, &key, portMAX_DELAY) != pdTRUE)
            continue;

        if (s_mode == MP_MODE_BLE) {
            if (!s_ble_conn || !s_hid_dev) {
                set_status(LV_SYMBOL_CLOSE " Not connected");
                continue;
            }
            if (key == MP_KEY_VIDEO_TOGGLE) {
                if (s_app == MP_APP_GMEET)
                    send_kbd_shortcut(HID_MOD_LCTRL, HID_KEY_E);
                else
                    send_kbd_shortcut(HID_MOD_LCTRL | HID_MOD_LSHIFT, HID_KEY_O);
            } else if (key == MP_KEY_MIC_TOGGLE) {
                if (s_app == MP_APP_GMEET)
                    send_kbd_shortcut(HID_MOD_LCTRL, HID_KEY_D);
                else
                    send_kbd_shortcut(HID_MOD_LCTRL | HID_MOD_LSHIFT, HID_KEY_M);
            } else if (key == MP_KEY_END_MEETING) {
                if (s_app == MP_APP_GMEET)
                    send_kbd_shortcut(HID_MOD_LCTRL, HID_KEY_W);
                else
                    send_kbd_shortcut(HID_MOD_LCTRL | HID_MOD_LSHIFT, HID_KEY_H);
            } else {
                send_cc_report(key);
                vTaskDelay(pdMS_TO_TICKS(50));
                uint8_t rel[HID_CC_RPT_LEN] = {0, 0};
                esp_hidd_dev_input_set(s_hid_dev, 0, HID_RPT_ID_CC,
                                       rel, HID_CC_RPT_LEN);
            }
            set_status(LV_SYMBOL_OK " Sent (BLE)");
        } else {
            if (key < MP_KEY_COUNT) {
                send_udp_key(s_udp_names[key]);
                set_status(LV_SYMBOL_OK " Sent (WiFi)");
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  LVGL overlay
 * ═══════════════════════════════════════════════════════════ */
static lv_obj_t  *s_overlay    = NULL;
static lv_obj_t  *s_status_lbl = NULL;
static lv_obj_t  *s_mode_lbl   = NULL;
static lv_obj_t  *s_app_lbl    = NULL;
static lv_obj_t  *s_mode_btn   = NULL;
static lv_obj_t  *s_app_btn_obj = NULL;
static lv_timer_t *s_status_tmr = NULL;

/* Toggle button state (default OFF — most meetings start muted/cam off) */
static bool       s_video_on   = false;
static bool       s_mic_on     = false;

/* Pending state from PC (written by UDP rx task, consumed by timer) */
static volatile int8_t s_pending_mic   = -1;  /* -1=none, 0=off, 1=on */
static volatile int8_t s_pending_video = -1;
static lv_obj_t  *s_video_btn  = NULL;
static lv_obj_t  *s_video_icon = NULL;
static lv_obj_t  *s_video_lbl  = NULL;
static lv_obj_t  *s_mic_btn    = NULL;
static lv_obj_t  *s_mic_icon   = NULL;
static lv_obj_t  *s_mic_lbl    = NULL;

/* Confirmation dialog */
static lv_obj_t  *s_confirm    = NULL;

static void update_toggle_visuals(lv_obj_t *btn, lv_obj_t *icon_lbl,
                                  lv_obj_t *text_lbl, bool on,
                                  const char *on_icon, const char *off_icon,
                                  const char *on_text, const char *off_text);

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_status_dirty && s_status_lbl) {
        lv_label_set_text(s_status_lbl, s_status_buf);
        s_status_dirty = false;
    }
    /* Apply pending state from PC */
    int8_t pm = s_pending_mic;
    if (pm >= 0 && s_mic_btn) {
        s_pending_mic = -1;
        bool new_state = (pm == 1);
        if (new_state != s_mic_on) {
            s_mic_on = new_state;
            update_toggle_visuals(s_mic_btn, s_mic_icon, s_mic_lbl, s_mic_on,
                                  LV_SYMBOL_CALL, LV_SYMBOL_CLOSE,
                                  "Mic On", "Mic Mute");
        }
    }
    int8_t pv = s_pending_video;
    if (pv >= 0 && s_video_btn) {
        s_pending_video = -1;
        bool new_state = (pv == 1);
        if (new_state != s_video_on) {
            s_video_on = new_state;
            update_toggle_visuals(s_video_btn, s_video_icon, s_video_lbl, s_video_on,
                                  LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EYE_CLOSE,
                                  "Video On", "Video Off");
        }
    }
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    macropad_close();
}

static void mode_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_mode = (s_mode == MP_MODE_BLE) ? MP_MODE_WIFI : MP_MODE_BLE;

    if (s_mode_lbl) {
        lv_label_set_text(s_mode_lbl,
                          s_mode == MP_MODE_BLE ? LV_SYMBOL_BLUETOOTH
                                                : LV_SYMBOL_WIFI);
    }
    /* Update button color */
    if (s_mode_btn) {
        lv_obj_set_style_bg_color(s_mode_btn,
            lv_color_hex(s_mode == MP_MODE_BLE ? 0x1565C0 : 0x2E7D32), 0);
    }
    if (s_status_lbl) {
        if (s_mode == MP_MODE_BLE) {
            lv_label_set_text(s_status_lbl,
                              s_ble_conn ? LV_SYMBOL_BLUETOOTH " Connected"
                                         : LV_SYMBOL_BLUETOOTH " Advertising...");
        } else {
            lv_label_set_text(s_status_lbl, LV_SYMBOL_WIFI " UDP Broadcast");
        }
    }
}

static void app_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_app = (s_app == MP_APP_GMEET) ? MP_APP_TEAMS : MP_APP_GMEET;
    if (s_app_lbl) {
        lv_label_set_text(s_app_lbl,
                          s_app == MP_APP_GMEET ? "Meet" : "Teams");
    }
    if (s_app_btn_obj) {
        lv_obj_set_style_bg_color(s_app_btn_obj,
            lv_color_hex(s_app == MP_APP_GMEET ? 0x0D47A1 : 0x4527A0), 0);
    }
}

static void vol_up_cb(lv_event_t *e)
{
    (void)e;
    mp_key_t key = MP_KEY_VOL_UP;
    xQueueSend(s_key_queue, &key, 0);
}

static void vol_down_cb(lv_event_t *e)
{
    (void)e;
    mp_key_t key = MP_KEY_VOL_DOWN;
    xQueueSend(s_key_queue, &key, 0);
}

static void update_toggle_visuals(lv_obj_t *btn, lv_obj_t *icon_lbl,
                                  lv_obj_t *text_lbl, bool on,
                                  const char *on_icon, const char *off_icon,
                                  const char *on_text, const char *off_text)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? 0x1B5E20 : 0x8B0000), 0);
    lv_label_set_text(icon_lbl, on ? on_icon : off_icon);
    lv_label_set_text(text_lbl, on ? on_text : off_text);
}

static void video_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_video_on = !s_video_on;
    update_toggle_visuals(s_video_btn, s_video_icon, s_video_lbl, s_video_on,
                          LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EYE_CLOSE,
                          "Video On", "Video Off");
    mp_key_t key = MP_KEY_VIDEO_TOGGLE;
    xQueueSend(s_key_queue, &key, 0);
}

/* Long-press: sync visual state without sending keystroke */
static void video_sync_cb(lv_event_t *e)
{
    (void)e;
    s_video_on = !s_video_on;
    update_toggle_visuals(s_video_btn, s_video_icon, s_video_lbl, s_video_on,
                          LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EYE_CLOSE,
                          "Video On", "Video Off");
    set_status("Synced video state");
}

static void mic_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_mic_on = !s_mic_on;
    update_toggle_visuals(s_mic_btn, s_mic_icon, s_mic_lbl, s_mic_on,
                          LV_SYMBOL_CALL, LV_SYMBOL_CLOSE,
                          "Mic On", "Mic Mute");
    mp_key_t key = MP_KEY_MIC_TOGGLE;
    xQueueSend(s_key_queue, &key, 0);
}

/* Long-press: sync visual state without sending keystroke */
static void mic_sync_cb(lv_event_t *e)
{
    (void)e;
    s_mic_on = !s_mic_on;
    update_toggle_visuals(s_mic_btn, s_mic_icon, s_mic_lbl, s_mic_on,
                          LV_SYMBOL_CALL, LV_SYMBOL_CLOSE,
                          "Mic On", "Mic Mute");
    set_status("Synced mic state");
}

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm) { lv_obj_del(s_confirm); s_confirm = NULL; }
    mp_key_t key = MP_KEY_END_MEETING;
    xQueueSend(s_key_queue, &key, 0);
}

static void confirm_no_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm) { lv_obj_del(s_confirm); s_confirm = NULL; }
}

static void end_meeting_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm) return;  /* already showing */

    s_confirm = lv_obj_create(s_overlay);
    lv_obj_set_size(s_confirm, 300, 110);
    lv_obj_align(s_confirm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_confirm, 12, 0);
    lv_obj_set_style_border_color(s_confirm, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_border_width(s_confirm, 2, 0);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(s_confirm);
    lv_label_set_text(msg, LV_SYMBOL_WARNING " End Meeting?");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFF6666), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *yes = lv_btn_create(s_confirm);
    lv_obj_set_size(yes, 110, 38);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 20, -10);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_radius(yes, 8, 0);
    lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, "Yes, End");
    lv_obj_set_style_text_color(yl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(yl, &lv_font_montserrat_14, 0);
    lv_obj_center(yl);

    lv_obj_t *no = lv_btn_create(s_confirm);
    lv_obj_set_size(no, 110, 38);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
    lv_obj_set_style_bg_color(no, lv_color_hex(0x444466), 0);
    lv_obj_set_style_radius(no, 8, 0);
    lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
    lv_obj_center(nl);
}

/* ═══════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════ */

/* UDP listener — receives state feedback from PC ("mic:0", "video:1", etc.) */
static void state_rx_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "state_rx socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(MP_UDP_RX_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "state_rx bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    char buf[64];
    while (1) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        buf[n] = '\0';
        /* Expected: "mic:0", "mic:1", "video:0", "video:1" */
        if      (strncmp(buf, "mic:0",   5) == 0) s_pending_mic   = 0;
        else if (strncmp(buf, "mic:1",   5) == 0) s_pending_mic   = 1;
        else if (strncmp(buf, "video:0", 7) == 0) s_pending_video = 0;
        else if (strncmp(buf, "video:1", 7) == 0) s_pending_video = 1;
        ESP_LOGI(TAG, "state_rx: %s", buf);
    }
}

void macropad_init(void)
{
    /* --- Key-send task & queue (transport-agnostic) -------- */
    if (!s_key_queue) {
        s_key_queue = xQueueCreate(8, sizeof(mp_key_t));
        xTaskCreatePinnedToCore(key_send_task, "mp_send", 3072,
                                NULL, 5, &s_send_task, tskNO_AFFINITY);
    }

    /* --- State-receive task (PC → ESP32 via UDP) ----------- */
    if (!s_rx_task) {
        xTaskCreatePinnedToCore(state_rx_task, "mp_rx", 3072,
                                NULL, 4, &s_rx_task, tskNO_AFFINITY);
    }

    /* --- BLE HID initialisation --------------------------- */
    if (s_ble_inited) return;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return;
    }

    /* Security: no-IO pairing, bonding enabled */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    /* Register HID device (GATT services created here) */
    ret = esp_hidd_dev_init(&s_hid_cfg, ESP_HID_TRANSPORT_BLE,
                            hidd_event_cb, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init: %s", esp_err_to_name(ret));
        return;
    }

    /* Prepare advertising data */
    adv_fields_setup();

    /* Set GAP device name */
    ble_svc_gap_device_name_set(MP_DEVICE_NAME);

    /* NimBLE bonding store */
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Start NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    s_ble_inited = true;
    ESP_LOGI(TAG, "Macropad BLE HID initialised");
}

void macropad_open(lv_obj_t *parent)
{
    if (s_overlay) return;

    /* Full-screen overlay */
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Back button (top-left) ------------------------ */
    lv_obj_t *back = lv_btn_create(s_overlay);
    lv_obj_set_size(back, 56, 26);
    lv_obj_set_pos(back, 6, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x333344), 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_add_event_cb(back, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* --- Title (center-top) ---------------------------- */
    lv_obj_t *title = lv_label_create(s_overlay);
    lv_label_set_text(title, LV_SYMBOL_KEYBOARD " Macropad");
    lv_obj_set_style_text_color(title, lv_color_hex(0xDDDDFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* --- Status label (top-right corner) --------------- */
    s_status_lbl = lv_label_create(s_overlay);
    if (s_mode == MP_MODE_BLE) {
        lv_label_set_text(s_status_lbl,
                          s_ble_conn ? LV_SYMBOL_BLUETOOTH " Connected"
                                     : LV_SYMBOL_BLUETOOTH " Advertising...");
    } else {
        lv_label_set_text(s_status_lbl, LV_SYMBOL_WIFI " UDP Broadcast");
    }
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x667788), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_RIGHT, -6, 8);

    /* ═══════════════════════════════════════════════════
     *  Main button area — absolute positioning
     *  Layout: [App] [Vol+/-] [Mic] [Video] [End] [Mode]
     *  y starts at 34, available height ~130px
     * ═══════════════════════════════════════════════════ */
    const int by = 34;   /* button area top */
    const int bh = 130;  /* total button height */
    int x = 20;

    /* --- Meet/Teams toggle (far left) ------------------ */
    s_app_btn_obj = lv_btn_create(s_overlay);
    lv_obj_set_size(s_app_btn_obj, 60, bh);
    lv_obj_set_pos(s_app_btn_obj, x, by);
    lv_obj_set_style_bg_color(s_app_btn_obj,
                              lv_color_hex(s_app == MP_APP_GMEET ? 0x0D47A1 : 0x4527A0), 0);
    lv_obj_set_style_bg_opa(s_app_btn_obj, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_app_btn_obj, 10, 0);
    lv_obj_add_event_cb(s_app_btn_obj, app_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_app_lbl = lv_label_create(s_app_btn_obj);
    lv_label_set_text(s_app_lbl,
                      s_app == MP_APP_GMEET ? "Meet" : "Teams");
    lv_obj_set_style_text_font(s_app_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_app_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_app_lbl);

    x += 60 + 6;

    /* --- Vol+/Vol- stacked ----------------------------- */
    int vol_h = (bh - 6) / 2;

    lv_obj_t *vol_up = lv_btn_create(s_overlay);
    lv_obj_set_size(vol_up, 60, vol_h);
    lv_obj_set_pos(vol_up, x, by);
    lv_obj_set_style_bg_color(vol_up, lv_color_hex(0x4A148C), 0);
    lv_obj_set_style_bg_opa(vol_up, LV_OPA_90, 0);
    lv_obj_set_style_radius(vol_up, 10, 0);
    lv_obj_add_event_cb(vol_up, vol_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vu_icon = lv_label_create(vol_up);
    lv_label_set_text(vu_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vu_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(vu_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(vu_icon);

    lv_obj_t *vol_dn = lv_btn_create(s_overlay);
    lv_obj_set_size(vol_dn, 60, vol_h);
    lv_obj_set_pos(vol_dn, x, by + vol_h + 6);
    lv_obj_set_style_bg_color(vol_dn, lv_color_hex(0x4A148C), 0);
    lv_obj_set_style_bg_opa(vol_dn, LV_OPA_90, 0);
    lv_obj_set_style_radius(vol_dn, 10, 0);
    lv_obj_add_event_cb(vol_dn, vol_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vd_icon = lv_label_create(vol_dn);
    lv_label_set_text(vd_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_font(vd_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(vd_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(vd_icon);

    x += 60 + 6;

    /* --- Mic toggle ------------------------------------ */
    s_mic_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_mic_btn, 150, bh);
    lv_obj_set_pos(s_mic_btn, x, by);
    lv_obj_set_style_bg_color(s_mic_btn,
                              lv_color_hex(s_mic_on ? 0x1B5E20 : 0x8B0000), 0);
    lv_obj_set_style_bg_opa(s_mic_btn, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_mic_btn, 10, 0);
    lv_obj_add_event_cb(s_mic_btn, mic_toggle_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_mic_btn, mic_sync_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_mic_icon = lv_label_create(s_mic_btn);
    lv_label_set_text(s_mic_icon, s_mic_on ? LV_SYMBOL_CALL : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(s_mic_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_mic_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_mic_icon, LV_ALIGN_CENTER, 0, -14);
    s_mic_lbl = lv_label_create(s_mic_btn);
    lv_label_set_text(s_mic_lbl, s_mic_on ? "Mic On" : "Mic Mute");
    lv_obj_set_style_text_font(s_mic_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_mic_lbl, lv_color_hex(0xCCCCDD), 0);
    lv_obj_align(s_mic_lbl, LV_ALIGN_CENTER, 0, 16);

    x += 150 + 6;

    /* --- Video toggle ---------------------------------- */
    s_video_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_video_btn, 150, bh);
    lv_obj_set_pos(s_video_btn, x, by);
    lv_obj_set_style_bg_color(s_video_btn,
                              lv_color_hex(s_video_on ? 0x1B5E20 : 0x8B0000), 0);
    lv_obj_set_style_bg_opa(s_video_btn, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_video_btn, 10, 0);
    lv_obj_add_event_cb(s_video_btn, video_toggle_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_video_btn, video_sync_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_video_icon = lv_label_create(s_video_btn);
    lv_label_set_text(s_video_icon,
                      s_video_on ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(s_video_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_video_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_video_icon, LV_ALIGN_CENTER, 0, -14);
    s_video_lbl = lv_label_create(s_video_btn);
    lv_label_set_text(s_video_lbl, s_video_on ? "Video On" : "Video Off");
    lv_obj_set_style_text_font(s_video_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_video_lbl, lv_color_hex(0xCCCCDD), 0);
    lv_obj_align(s_video_lbl, LV_ALIGN_CENTER, 0, 16);

    x += 150 + 6;

    /* --- End Meeting ----------------------------------- */
    lv_obj_t *end_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(end_btn, 90, bh);
    lv_obj_set_pos(end_btn, x, by);
    lv_obj_set_style_bg_color(end_btn, lv_color_hex(0x8B0000), 0);
    lv_obj_set_style_bg_opa(end_btn, LV_OPA_90, 0);
    lv_obj_set_style_radius(end_btn, 10, 0);
    lv_obj_set_style_border_color(end_btn, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_border_width(end_btn, 2, 0);
    lv_obj_add_event_cb(end_btn, end_meeting_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *end_icon = lv_label_create(end_btn);
    lv_label_set_text(end_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(end_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(end_icon, lv_color_hex(0xFF4444), 0);
    lv_obj_align(end_icon, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *end_lbl = lv_label_create(end_btn);
    lv_label_set_text(end_lbl, "End");
    lv_obj_set_style_text_font(end_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(end_lbl, lv_color_hex(0xFF8888), 0);
    lv_obj_align(end_lbl, LV_ALIGN_CENTER, 0, 16);

    x += 90 + 6;

    /* --- BLE/WiFi toggle (far right) ------------------- */
    s_mode_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(s_mode_btn, 60, bh);
    lv_obj_set_pos(s_mode_btn, x, by);
    lv_obj_set_style_bg_color(s_mode_btn,
                              lv_color_hex(s_mode == MP_MODE_BLE ? 0x1565C0 : 0x00695C), 0);
    lv_obj_set_style_bg_opa(s_mode_btn, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_mode_btn, 10, 0);
    lv_obj_add_event_cb(s_mode_btn, mode_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_mode_lbl = lv_label_create(s_mode_btn);
    lv_label_set_text(s_mode_lbl,
                      s_mode == MP_MODE_BLE ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_mode_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_mode_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_mode_lbl);

    /* Periodic timer to push BLE status updates into LVGL */
    s_status_tmr = lv_timer_create(status_timer_cb, 250, NULL);

    ESP_LOGI(TAG, "Macropad overlay opened (meeting mode)");
}

void macropad_close(void)
{
    if (s_confirm) {
        lv_obj_del(s_confirm);
        s_confirm = NULL;
    }
    if (s_status_tmr) {
        lv_timer_del(s_status_tmr);
        s_status_tmr = NULL;
    }
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay     = NULL;
        s_status_lbl  = NULL;
        s_mode_lbl    = NULL;
        s_mode_btn    = NULL;
        s_app_lbl     = NULL;
        s_app_btn_obj = NULL;
        s_video_btn   = NULL;
        s_video_icon = NULL;
        s_video_lbl  = NULL;
        s_mic_btn    = NULL;
        s_mic_icon   = NULL;
        s_mic_lbl    = NULL;
    }
}

bool macropad_is_active(void)
{
    return s_overlay != NULL;
}

bool macropad_ble_inited(void)
{
    return s_ble_inited;
}
