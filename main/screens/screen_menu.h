#ifndef SCREEN_MENU_H
#define SCREEN_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

void screen_menu_create(void);
void screen_menu_destroy(void);
void screen_menu_update(void);

/** Request that the WiFi / BLE overlay opens when the menu screen is shown */
void screen_menu_request_wifi(void);
void screen_menu_request_ble(void);

#ifdef __cplusplus
}
#endif

#endif
