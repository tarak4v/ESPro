#ifndef SCREEN_ASSISTANT_H
#define SCREEN_ASSISTANT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the Siri-style AI assistant overlay on @p parent screen. */
void screen_assistant_open(lv_obj_t *parent);

/** Close the assistant overlay and free resources. */
void screen_assistant_close(void);

/** Periodic tick — drives waveform, state transitions, playback. */
void screen_assistant_update(void);

/** True while the assistant overlay is visible. */
bool screen_assistant_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_ASSISTANT_H */
