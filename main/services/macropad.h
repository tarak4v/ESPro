/**
 * @file macropad.h
 * @brief BLE HID + WiFi UDP macropad — media control without extra hardware.
 *
 * Two transport modes:
 *  - BLE HID: Watch acts as a Bluetooth Consumer-Control device.
 *             PC pairs directly, no dongle needed.
 *  - WiFi UDP: Sends key name strings over UDP broadcast (port 13579)
 *             to a PC listener script.
 */

#ifndef MACROPAD_H
#define MACROPAD_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise BLE HID stack (call once early, before any BLE use). */
void macropad_init(void);

/** Open the macropad overlay on the given parent screen. */
void macropad_open(lv_obj_t *parent);

/** Close the overlay. */
void macropad_close(void);

/** Returns true while overlay is visible. */
bool macropad_is_active(void);

/** Returns true if NimBLE was already initialised by macropad. */
bool macropad_ble_inited(void);

#ifdef __cplusplus
}
#endif

#endif /* MACROPAD_H */
