#ifndef MIC_INPUT_H
#define MIC_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise ES7210 ADC + I2S full-duplex channel (TX + RX on I2S_NUM_0).
 * Call once after I2C bus is ready.
 */
void mic_init(void);

/**
 * Return the shared I2S TX handle created by mic_init().
 * Used by boot_sound and music_player for speaker output.
 */
i2s_chan_handle_t mic_get_shared_i2s_tx(void);

/**
 * Start reading audio samples from the microphone.
 * Spawns a background task that continuously reads and computes RMS level.
 */
void mic_start(void);

/**
 * Stop the microphone reading task and release I2S RX channel.
 */
void mic_stop(void);

/** True if mic is currently active and sampling. */
bool mic_is_active(void);

/**
 * Current audio level (RMS) as 0–100 scale.
 * Returns 0 if mic is not active.
 */
uint8_t mic_get_level(void);

/**
 * Set a PSRAM buffer for concurrent recording.
 * While set, the mic task copies raw PCM into this buffer.
 * @param buf      Pointer to int16_t buffer (PSRAM recommended).
 * @param max_bytes  Maximum buffer size in bytes.
 */
void mic_set_record_buffer(int16_t *buf, size_t max_bytes);

/** Return number of bytes written to the recording buffer so far. */
size_t mic_get_recorded_bytes(void);

/** Clear the recording buffer pointer (stop recording). */
void mic_clear_record_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* MIC_INPUT_H */
