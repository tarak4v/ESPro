#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the internet-radio music player overlay. */
void music_player_open(lv_obj_t *parent);

/** Close and stop playback. */
void music_player_close(void);

/** Periodic tick — drives deferred state changes and UI updates. */
void music_player_update(void);

/** Returns true while the music overlay is active. */
bool music_player_is_active(void);

/** Apply volume (0-255) to the active codec device, if playing. */
void music_player_set_volume(uint8_t vol);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H */
