#ifndef GAME_MAZE_H
#define GAME_MAZE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the maze game overlay on the given parent screen.
 * @param parent  The menu screen object to parent the overlay onto.
 */
void game_maze_open(lv_obj_t *parent);

/**
 * Called periodically (~100 ms) from screen_menu_update().
 * Drives ball physics, collision detection, and timer display.
 */
void game_maze_update(void);

/**
 * Close and clean up the maze game overlay (if open).
 */
void game_maze_close(void);

/**
 * Returns true while the game overlay is active.
 */
bool game_maze_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GAME_MAZE_H */
