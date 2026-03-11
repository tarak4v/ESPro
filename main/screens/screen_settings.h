#ifndef SCREEN_SETTINGS_H
#define SCREEN_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void screen_settings_create(void);
void screen_settings_destroy(void);

/* Load persistent settings from NVS (call early in boot) */
void settings_load_from_nvs(void);

/* Persistent settings — written by settings screen, read by others */
extern uint8_t  g_volume;          /* 0-255, ES8311 DAC reg 0x32 */
extern bool     g_boot_sound_en;   /* boot chime on/off */
extern bool     g_clock_24h;       /* false = 12h, true = 24h */
extern bool     g_theme_dark;      /* true = dark (default), false = light */
extern bool     g_clock_flip;      /* true = flip clock, false = digital */

/* Theme colour palette (hex values — use lv_color_hex() to convert) */
extern uint32_t th_bg;             /* screen background */
extern uint32_t th_card;           /* glass/card panel */
extern uint32_t th_text;           /* primary text */
extern uint32_t th_label;          /* secondary/label text */
extern uint32_t th_btn;            /* button background */

/** Recompute theme palette from g_theme_dark. */
void theme_apply(void);

#ifdef __cplusplus
}
#endif

#endif
