#ifndef GAME_DINO_H
#define GAME_DINO_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void game_dino_open(lv_obj_t *parent);
void game_dino_update(void);
void game_dino_close(void);
bool game_dino_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GAME_DINO_H */
