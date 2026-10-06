#include "audio_agc.h"
#include "app_config.h"

void audio_agc_init(audio_agc_t *agc)
{
    if (!agc) return;
    agc->target_level = AGC_TARGET_LEVEL;
    agc->decay_rate = AGC_DECAY_RATE;
    agc->max_gain = AGC_MAX_GAIN;
    agc->noise_floor = AGC_NOISE_FLOOR;
    agc->peak = agc->target_level; // Soft-start at 1.0x gain to prevent burst noise
}

void audio_agc_reset(audio_agc_t *agc)
{
    if (!agc) return;
    agc->peak = agc->target_level;
}

void audio_agc_process(audio_agc_t *agc, int16_t *samples, size_t count)
{
    if (!agc || !samples || count == 0) return;

    float peak = agc->peak;
    float target = agc->target_level;
    float decay = agc->decay_rate;
    float max_gain = agc->max_gain;
    float min_gain = AGC_MIN_GAIN;
    float floor_val = agc->noise_floor;

    for (size_t i = 0; i < count; i++) {
        float v = (float)samples[i];
        float a = (v < 0.0f) ? -v : v;

        if (a > peak) {
            peak = a;
        } else {
            peak *= decay;
        }

        float denom = (peak > floor_val) ? peak : floor_val;
        float gain = target / denom;
        // Clamp from both ends. The upper clamp is the important one: without it
        // the gain pins at its maximum whenever the envelope falls to the noise
        // floor, which amplifies hiss by that factor during every pause. The
        // lower clamp keeps a loud syllable from being ducked into inaudibility
        // and from pumping the level on the syllable that follows it.
        if (gain > max_gain) {
            gain = max_gain;
        } else if (gain < min_gain) {
            gain = min_gain;
        }

        v *= gain;

        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }

        samples[i] = (int16_t)v;
    }

    agc->peak = peak;
}
