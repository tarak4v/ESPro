#ifndef SCREEN_CLOCK_H
#define SCREEN_CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

void screen_clock_create(void);
void screen_clock_destroy(void);
void screen_clock_update(void);
void pcf85063_init(void);

#ifdef __cplusplus
}
#endif

#endif
