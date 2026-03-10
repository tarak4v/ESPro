#ifndef SCREEN_TAMAFI_H
#define SCREEN_TAMAFI_H

#ifdef __cplusplus
extern "C" {
#endif

void screen_tamafi_create(void);
void screen_tamafi_destroy(void);
void screen_tamafi_update(void);

/** Load pet state from NVS (call early in boot) */
void tamafi_load_from_nvs(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_TAMAFI_H */
