#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROV_IDLE,
    PROV_WAITING,       /* AP started, waiting for phone */
    PROV_SAVED,         /* credentials saved via portal */
    PROV_CONNECTING,    /* trying STA connection with new creds */
    PROV_SUCCESS,       /* STA connected */
    PROV_FAILED,        /* STA connection failed */
} wifi_prov_status_t;

/** Start SoftAP "ESPro-Setup" + captive portal HTTP server + DNS. */
void wifi_prov_start(void);

/** Stop AP, HTTP server, DNS; switch back to STA with saved creds. */
void wifi_prov_stop(void);

/** True while provisioning AP is active. */
bool wifi_prov_is_active(void);

/** Current provisioning status. */
wifi_prov_status_t wifi_prov_get_status(void);

/** Number of stations connected to our AP. */
int wifi_prov_get_ap_clients(void);

/** The SSID most recently saved via the portal (empty if none). */
const char *wifi_prov_get_saved_ssid(void);

/**
 * Read stored WiFi credentials from NVS.
 * Returns true if valid SSID was found.
 */
bool wifi_prov_load_creds(char *ssid, size_t ssid_len,
                          char *pass, size_t pass_len);

/**
 * Read stored weather city from NVS "wifi_cfg" namespace.
 * Falls back to WEATHER_CITY from hw_config.h.
 */
void wifi_prov_load_city(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROV_H */
