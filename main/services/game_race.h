#ifndef GAME_RACE_H
#define GAME_RACE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void game_race_open(lv_obj_t *parent);
void game_race_update(void);
void game_race_close(void);
bool game_race_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GAME_RACE_H */
