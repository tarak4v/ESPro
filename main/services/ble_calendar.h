/**
 * @file ble_calendar.h
 * @brief BLE GATT server for receiving calendar events from phone companion app.
 */
#ifndef BLE_CALENDAR_H
#define BLE_CALENDAR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAL_EVENT_MAX    8
#define CAL_TITLE_LEN    48

typedef struct {
    char     title[CAL_TITLE_LEN];
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  day;                /* day of month */
    uint8_t  month;
    bool     valid;
} cal_event_t;

/** Global calendar events (sorted by time). */
extern cal_event_t g_cal_events[CAL_EVENT_MAX];
extern uint8_t     g_cal_count;

/** Initialise BLE calendar GATT service. Call after macropad_init(). */
void ble_calendar_init(void);

/** Load cached events from LittleFS. */
void ble_calendar_load(void);

/** Save current events to LittleFS. */
void ble_calendar_save(void);

/** Get the next upcoming event (or NULL if none). */
const cal_event_t *ble_calendar_next_event(void);

/** Manually add an event (e.g. from DeepSeek response). */
void ble_calendar_add(const char *title, uint8_t hour, uint8_t min,
                      uint8_t day, uint8_t month);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CALENDAR_H */
