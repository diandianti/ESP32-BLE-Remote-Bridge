#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t prev_decoded;
    int16_t last_sample;
    float   dc_x;
    float   dc_y;
} audio_filter_state_t;

void audio_filter_init(audio_filter_state_t *state);
void audio_filter_declip(audio_filter_state_t *state, int16_t *samples, size_t count, int16_t threshold);
void audio_filter_lowpass(audio_filter_state_t *state, int16_t *samples, size_t count);

/**
 * @brief One-pole high-pass section at AUDIO_HP_HZ.
 *
 * Call it twice in a row on the same buffer to get a second-order (12 dB/oct)
 * roll-off: the state is shared, so consecutive calls cascade correctly.
 */
void audio_filter_highpass(audio_filter_state_t *state, int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
