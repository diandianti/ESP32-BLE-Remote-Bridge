#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t prev_decoded;
    int16_t last_sample;
    // One (x, y) history pair per cascaded high-pass section. Sharing a single
    // pair between the two sections fed each one the other's history at every
    // block boundary, which re-injected the DC it was meant to remove as a
    // step once per BLE packet.
    float   hp_x[2];
    float   hp_y[2];
} audio_filter_state_t;

void audio_filter_init(audio_filter_state_t *state);
void audio_filter_declip(audio_filter_state_t *state, int16_t *samples, size_t count, int16_t threshold);
void audio_filter_lowpass(audio_filter_state_t *state, int16_t *samples, size_t count);

/**
 * @brief Speech high-pass at AUDIO_HP_HZ: two cascaded one-pole sections
 *        (12 dB/oct), each with its own history.
 */
void audio_filter_highpass(audio_filter_state_t *state, int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
