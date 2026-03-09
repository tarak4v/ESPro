#ifndef BOOT_SOUND_H
#define BOOT_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Play a short "ting" tone through the ES8311 codec + speaker.
 * Blocking — returns after the tone finishes (~400 ms).
 */
void boot_sound_play(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_SOUND_H */
