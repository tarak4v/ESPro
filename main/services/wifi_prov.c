/**
 * @file wifi_prov.c
 * @brief SoftAP + captive-portal HTTP server for WiFi provisioning.
 *
 * When started, the device broadcasts "ESPro-Setup" (open AP).
 * A minimal DNS server redirects all domains to 192.168.4.1 so
 * phones auto-open the captive portal page.
 *
 * The embedded HTML page lets the user:
 *   – scan for available WiFi networks
 *   – select a network and enter its password
 *   – optionally set the weather city
 *   – save everything to NVS ("wifi_cfg" namespace)
 */

#include "wifi_prov.h"
#include "hw_config.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "wifi_prov";

#define NVS_NS          "wifi_cfg"
#define AP_SSID         "ESPro-Setup"
#define AP_MAX_CONN     4
#define SCAN_MAX        20

/* ── State ────────────────────────────────────────────────── */
static volatile bool              s_active    = false;
static volatile wifi_prov_status_t s_status   = PROV_IDLE;
static esp_netif_t               *s_ap_netif  = NULL;
static httpd_handle_t             s_httpd     = NULL;
static TaskHandle_t               s_dns_task  = NULL;
static volatile bool              s_dns_run   = false;

static char  s_saved_ssid[33]  = "";
static char  s_saved_pass[65]  = "";
static bool  s_has_new_creds   = false;

/* ══════════════════════════════════════════════════════════════
 *  Captive-portal HTML  (≈ 2.5 KB, served for every GET)
 * ══════════════════════════════════════════════════════════════ */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESPro Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,sans-serif;background:#0a0a14;color:#fff;padding:16px}"
"h1{font-size:1.4em;margin-bottom:12px;color:#ff6644}"
".c{background:#1a1a2e;border-radius:12px;padding:14px;margin-bottom:12px}"
"h2{font-size:1em;margin-bottom:8px;color:#888}"
"#nets{max-height:220px;overflow-y:auto}"
".n{display:flex;align-items:center;padding:10px;border-radius:8px;cursor:pointer;margin:3px 0}"
".n:hover,.n.s{background:#333}"
".n .r{margin-left:auto;color:#888;font-size:.8em}"
".n .l{margin-right:8px;font-size:.8em}"
"input{width:100%;padding:10px;border:1px solid #444;border-radius:8px;"
"background:#222;color:#fff;font-size:1em;margin:6px 0}"
"button{padding:10px 16px;border:none;border-radius:8px;font-size:1em;cursor:pointer;margin:4px 0}"
".bs{background:#1a5a3a;color:#fff}"
".bv{background:#ff6644;color:#fff;width:100%;font-size:1.1em;padding:12px}"
"#msg{padding:8px;border-radius:8px;text-align:center;margin-top:8px}"
"</style></head><body>"
"<h1>&#128246; ESPro Setup</h1>"
"<div class=\"c\"><h2>WiFi Network</h2>"
"<button class=\"bs\" onclick=\"scan()\">&#128268; Scan Networks</button>"
"<div id=\"nets\"><p style=\"color:#888\">Tap Scan to find networks</p></div>"
"<input type=\"hidden\" id=\"ssid\">"
"<input type=\"password\" id=\"pass\" placeholder=\"WiFi Password\">"
"</div>"
"<div class=\"c\"><h2>Weather City</h2>"
"<input id=\"city\" placeholder=\"City,Country  e.g. Bengaluru,IN\" value=\"" WEATHER_CITY "\">"
"</div>"
"<button class=\"bv\" onclick=\"save()\">&#10004; Save &amp; Connect</button>"
"<div id=\"msg\"></div>"
"<script>"
"var ss='';"
"function scan(){"
"var d=document.getElementById('nets');"
"d.innerHTML='<p style=\"color:#fc0\">Scanning…</p>';"
"var x=new XMLHttpRequest();x.open('GET','/scan');"
"x.onload=function(){"
"var a=JSON.parse(x.responseText),h='';"
"for(var i=0;i<a.length;i++){"
"h+='<div class=\"n\" onclick=\"pk(this,\\''+a[i].s+'\\')\">'"
"+(a[i].a>0?'<span class=\"l\">&#128274;</span>':'')"
"+'<span>'+a[i].s+'</span>'"
"+'<span class=\"r\">'+a[i].r+' dBm</span></div>';}"
"d.innerHTML=h||'<p style=\"color:#888\">No networks found</p>';};"
"x.onerror=function(){d.innerHTML='<p style=\"color:#f44\">Scan failed</p>';};"
"x.send();}"
"function pk(el,s){ss=s;document.getElementById('ssid').value=s;"
"var ns=document.querySelectorAll('.n');for(var i=0;i<ns.length;i++)ns[i].classList.remove('s');"
"el.classList.add('s');}"
"function save(){"
"var s=document.getElementById('ssid').value,"
"p=document.getElementById('pass').value,"
"c=document.getElementById('city').value;"
"if(!s){document.getElementById('msg').innerHTML='<p style=\"color:#f44\">Select a network</p>';return;}"
"document.getElementById('msg').innerHTML='<p style=\"color:#fc0\">Saving…</p>';"
"var x=new XMLHttpRequest();x.open('POST','/save');"
"x.setRequestHeader('Content-Type','application/json');"
"x.onload=function(){"
"var r=JSON.parse(x.responseText);"
"document.getElementById('msg').innerHTML='<p style=\"color:#0f8\">'+r.msg+'</p>';};"
"x.onerror=function(){"
"document.getElementById('msg').innerHTML='<p style=\"color:#f44\">Error</p>';};"
"x.send(JSON.stringify({ssid:s,pass:p,city:c}));}"
"scan();"
"</script></body></html>";

/* ══════════════════════════════════════════════════════════════
 *  NVS helpers
 * ══════════════════════════════════════════════════════════════ */
static void nvs_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool wifi_prov_load_creds(char *ssid, size_t ssid_len,
                          char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = false;
    size_t len = ssid_len;
    if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && len > 1) {
        ok = true;
        len = pass_len;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK)
            pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

void wifi_prov_load_city(char *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = len;
        if (nvs_get_str(h, "city", buf, &sz) == ESP_OK && sz > 1) {
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    /* fallback */
    strncpy(buf, WEATHER_CITY, len - 1);
    buf[len - 1] = '\0';
}

/* ══════════════════════════════════════════════════════════════
 *  HTTP handlers
 * ══════════════════════════════════════════════════════════════ */

/* Serve HTML for every GET (catches captive-portal probes) */
static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
}

/* Captive portal detection: Android /generate_204 */
static esp_err_t captive_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Captive portal detection: iOS/macOS /hotspot-detect.html */
static esp_err_t captive_ios_handler(httpd_req_t *req)
{
    /* iOS expects non-"<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
       to trigger captive portal — redirect to our page. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Microsoft/Windows connectivity check */
static esp_err_t captive_msft_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /scan — return JSON array of APs */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    /* Kick off a blocking scan (APSTA mode allows it) */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 100, .max = 300 },
    };

    wifi_ap_record_t *aps = calloc(SCAN_MAX, sizeof(wifi_ap_record_t));
    if (!aps) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t count = SCAN_MAX;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   /* blocking */
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&count, aps);
    } else {
        count = 0;
    }

    /* Build JSON */
    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "s", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(obj, "r", aps[i].rssi);
        cJSON_AddNumberToObject(obj, "a",
            aps[i].authmode != WIFI_AUTH_OPEN ? 1 : 0);
        cJSON_AddItemToArray(arr, obj);
    }
    free(aps);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

/* POST /save — receive { ssid, pass, city } */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_FAIL;
    }

    char *body = malloc(total + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); httpd_resp_send_500(req); return ESP_FAIL; }
        received += r;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    cJSON *j_city = cJSON_GetObjectItem(root, "city");

    if (!cJSON_IsString(j_ssid) || strlen(j_ssid->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    /* Save to NVS */
    strncpy(s_saved_ssid, j_ssid->valuestring, sizeof(s_saved_ssid) - 1);
    s_saved_ssid[sizeof(s_saved_ssid) - 1] = '\0';
    nvs_save_str("ssid", s_saved_ssid);

    if (cJSON_IsString(j_pass)) {
        strncpy(s_saved_pass, j_pass->valuestring, sizeof(s_saved_pass) - 1);
        s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';
    } else {
        s_saved_pass[0] = '\0';
    }
    nvs_save_str("pass", s_saved_pass);

    if (cJSON_IsString(j_city) && strlen(j_city->valuestring) > 0) {
        nvs_save_str("city", j_city->valuestring);
    }

    s_has_new_creds = true;
    s_status = PROV_SAVED;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Saved creds for SSID: %s", s_saved_ssid);

    /* Respond */
    const char *resp = "{\"msg\":\"Saved! Go back to device to connect.\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, strlen(resp));
}

/* ══════════════════════════════════════════════════════════════
 *  DNS server — redirect all queries to 192.168.4.1
 * ══════════════════════════════════════════════════════════════ */
static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 1-second receive timeout so we can check s_dns_run */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS server started on :53");

    while (s_dns_run) {
        uint8_t buf[256];
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&cli, &cli_len);
        if (len < 12) continue;          /* timeout or too short */

        /* Turn query into a response */
        buf[2] = 0x81; buf[3] = 0x80;    /* flags: response + recursion */
        buf[6] = 0x00; buf[7] = 0x01;    /* 1 answer RR */

        /* Append answer: name-pointer + A record → 192.168.4.1 */
        uint8_t *p = buf + len;
        *p++ = 0xC0; *p++ = 0x0C;        /* pointer to question name */
        *p++ = 0x00; *p++ = 0x01;        /* type A */
        *p++ = 0x00; *p++ = 0x01;        /* class IN */
        *p++ = 0x00; *p++ = 0x00;
        *p++ = 0x00; *p++ = 0x3C;        /* TTL 60 s */
        *p++ = 0x00; *p++ = 0x04;        /* data length */
        *p++ = 192;  *p++ = 168;
        *p++ = 4;    *p++ = 1;

        sendto(sock, buf, (size_t)(p - buf), 0,
               (struct sockaddr *)&cli, cli_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  HTTP server start / stop
 * ══════════════════════════════════════════════════════════════ */
static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers   = 12;
    cfg.stack_size         = 8192;
    cfg.lru_purge_enable   = true;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* Captive portal detection endpoints (must come FIRST) */
    static const httpd_uri_t uri_204 = {
        .uri = "/generate_204", .method = HTTP_GET,
        .handler = captive_204_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_204);

    static const httpd_uri_t uri_gen204 = {
        .uri = "/gen_204", .method = HTTP_GET,
        .handler = captive_204_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_gen204);

    static const httpd_uri_t uri_ios = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET,
        .handler = captive_ios_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_ios);

    static const httpd_uri_t uri_msft = {
        .uri = "/connecttest.txt", .method = HTTP_GET,
        .handler = captive_msft_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_msft);

    static const httpd_uri_t uri_msft2 = {
        .uri = "/redirect", .method = HTTP_GET,
        .handler = captive_msft_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_msft2);

    /* /scan — WiFi scan results */
    static const httpd_uri_t uri_scan = {
        .uri     = "/scan",
        .method  = HTTP_GET,
        .handler = scan_get_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_scan);

    /* /save — save credentials */
    static const httpd_uri_t uri_save = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_save);

    /* Wildcard — captive portal HTML (must be registered LAST) */
    static const httpd_uri_t uri_portal = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = portal_get_handler,
    };
    httpd_register_uri_handler(s_httpd, &uri_portal);

    ESP_LOGI(TAG, "HTTP server started on :80");
}

/* ══════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════ */
void wifi_prov_start(void)
{
    if (s_active) return;

    s_active       = true;
    s_has_new_creds = false;
    s_saved_ssid[0] = '\0';
    s_saved_pass[0] = '\0';
    s_status       = PROV_WAITING;

    /* Create AP netif once */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Disconnect STA so the phone stays on our AP (no internet bridge).
       Scanning still works in APSTA mode even without STA connection. */
    esp_wifi_disconnect();

    /* Switch to AP+STA (STA needed for scanning) */
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = sizeof(AP_SSID) - 1,
            .channel        = 1,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    ESP_LOGI(TAG, "SoftAP \"%s\" started", AP_SSID);

    /* HTTP server */
    start_http_server();

    /* DNS server */
    s_dns_run = true;
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);
}

void wifi_prov_stop(void)
{
    if (!s_active) return;

    /* Stop HTTP */
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    /* Stop DNS */
    s_dns_run = false;
    if (s_dns_task) {
        /* Task self-deletes after loop exits (≤1 s) */
        vTaskDelay(pdMS_TO_TICKS(1200));
        s_dns_task = NULL;
    }

    /* Back to STA only */
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Reconnect with new or existing credentials */
    if (s_has_new_creds && s_saved_ssid[0]) {
        s_status = PROV_CONNECTING;
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, s_saved_ssid,
                sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, s_saved_pass,
                sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode =
            s_saved_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Reconnecting STA to \"%s\"", s_saved_ssid);
    } else {
        esp_wifi_connect();
    }

    s_active = false;
    /* s_status left as PROV_CONNECTING or set externally */
}

bool wifi_prov_is_active(void)
{
    return s_active;
}

wifi_prov_status_t wifi_prov_get_status(void)
{
    return s_status;
}

int wifi_prov_get_ap_clients(void)
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK)
        return sta_list.num;
    return 0;
}

const char *wifi_prov_get_saved_ssid(void)
{
    return s_saved_ssid;
}
