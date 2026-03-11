#ifndef WIFI_TIME_H
#define WIFI_TIME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to WiFi, obtain NTP time, write it to the PCF85063 RTC,
 * then optionally stay connected.
 *
 * Blocks until WiFi connects (with timeout) and NTP sync completes.
 */
void wifi_time_init(void);

/**
 * Returns true if WiFi STA is currently connected.
 */
bool wifi_is_connected(void);

/**
 * Returns the SSID currently in use (from NVS or hw_config default).
 */
const char *wifi_get_current_ssid(void);

/**
 * Reset the WiFi retry counter (call after provisioning).
 */
void wifi_reset_retry(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_TIME_H */
