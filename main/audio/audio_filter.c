#include "audio_filter.h"
#include <stdlib.h>

void audio_filter_init(audio_filter_state_t *state)
{
    if (!state) return;
    state->prev_decoded = 0;
    state->last_sample = 0;
    state->dc_x = 0.0f;
    state->dc_y = 0.0f;
}

void audio_filter_declip(audio_filter_state_t *state, int16_t *samples, size_t count, int16_t threshold)
{
    if (!state || !samples || count == 0) return;

    int32_t prev = state->prev_decoded;
    for (size_t i = 0; i < count; i++) {
        int32_t p = (i == 0) ? prev : samples[i - 1];
        int32_t nx = (i == count - 1) ? samples[i] : samples[i + 1];
        int32_t cur = samples[i];

        int32_t dp = abs(cur - p);
        int32_t dn = abs(cur - nx);
        int32_t nd = abs(nx - p);
        int32_t min_d = (dp < dn) ? dp : dn;

        if (dp > threshold && dn > threshold && min_d > nd * 2) {
            samples[i] = (int16_t)((p + nx) / 2);
        }
    }
    state->prev_decoded = samples[count - 1];
}

void audio_filter_lowpass(audio_filter_state_t *state, int16_t *samples, size_t count)
{
    if (!state || !samples || count == 0) return;

    int32_t prev = state->last_sample;
    for (size_t i = 0; i + 1 < count; i++) {
        int32_t cur = samples[i];
        samples[i] = (int16_t)((prev + (cur * 2) + samples[i + 1]) >> 2);
        prev = cur;
    }
    samples[count - 1] = (int16_t)((prev + (samples[count - 1] * 3)) >> 2);
    state->last_sample = samples[count - 1];
}

void audio_filter_dc_block(audio_filter_state_t *state, int16_t *samples, size_t count)
{
    if (!state || !samples || count == 0) return;

    float y_prev = state->dc_y;
    float x_prev = state->dc_x;
    const float R = 0.985f; // ~19 Hz high-pass at 8 kHz (scales with the sample rate)

    for (size_t i = 0; i < count; i++) {
        float x = (float)samples[i];
        float y = x - x_prev + R * y_prev;
        x_prev = x;
        y_prev = y;

        if (y > 32767.0f) y = 32767.0f;
        else if (y < -32768.0f) y = -32768.0f;
        samples[i] = (int16_t)y;
    }

    state->dc_x = x_prev;
    state->dc_y = y_prev;
}
