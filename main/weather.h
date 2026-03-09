#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    temp;              /* °C */
    int      humidity;          /* % */
    char     description[32];   /* e.g. "Clear", "Clouds" */
    bool     valid;
} weather_data_t;

/**
 * Start background task that fetches weather every 10 minutes.
 */
void weather_init(void);

/**
 * Thread-safe copy of the latest weather data.
 */
weather_data_t weather_get(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_H */
