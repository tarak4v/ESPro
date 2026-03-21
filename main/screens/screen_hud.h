/**
 * @file screen_hud.h
 * @brief Productivity HUD screen — 5-zone layout for the swipe cycle.
 */
#ifndef SCREEN_HUD_H
#define SCREEN_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

void screen_hud_create(void);
void screen_hud_update(void);
void screen_hud_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_HUD_H */
