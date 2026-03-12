#ifndef BOOT_SOUND_H
#define BOOT_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Single tone note: frequency (Hz) + duration (ms). freq=0 is a rest. */
typedef struct {
    uint16_t freq_hz;
    uint16_t dur_ms;
} tone_note_t;

/**
 * Play a short "ting" tone through the ES8311 codec + speaker.
 * Blocking — returns after the tone finishes (~400 ms).
 */
void boot_sound_play(void);

/** Play an arbitrary melody (blocking). Skips if music player is active.
 *  volume: ES8311 DAC volume 0-255 (use g_volume for user setting). */
void play_melody(const tone_note_t *notes, int count, uint8_t volume);

/** Non-blocking version — spawns a FreeRTOS task. */
void play_melody_async(const tone_note_t *notes, int count, uint8_t volume);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_SOUND_H */
