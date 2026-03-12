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

/* ── Watch face skins (stored on LittleFS) ─────────────── */
#define WATCH_FACE_COUNT 5

typedef struct {
    char     name[24];
    uint32_t time_d,   time_l;       /* time digits */
    uint32_t ampm_d,   ampm_l;       /* AM/PM label */
    uint32_t date_d,   date_l;       /* date text */
    uint32_t weekday_d, weekday_l;   /* weekday text */
    uint32_t wtemp_d,  wtemp_l;      /* weather temperature */
    uint32_t wdesc_d,  wdesc_l;      /* weather description */
    uint32_t wloc_d,   wloc_l;       /* weather location */
    uint32_t accent_d, accent_l;     /* dots, highlights */
    uint32_t glass_border_d, glass_border_l;
    uint32_t icon_d,   icon_l;       /* status bar icons default */
} watch_face_t;

extern uint8_t      g_watch_face;    /* 0 .. WATCH_FACE_COUNT-1 */
extern watch_face_t g_face;          /* currently loaded face */

/** Write default face JSON files to LittleFS if missing. */
void face_init_defaults(void);

/** Load face colors from LittleFS JSON file into g_face. */
void face_load(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif
