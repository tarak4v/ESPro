#ifndef POMODORO_H
#define POMODORO_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void pomodoro_open(lv_obj_t *parent);
void pomodoro_close(void);
void pomodoro_update(void);
bool pomodoro_is_active(void);

#ifdef __cplusplus
}
#endif
#endif /* POMODORO_H */
