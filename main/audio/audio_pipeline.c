#include "audio_pipeline.h"
#include <string.h>
#include "app_log.h"
#include "esp_timer.h"

audio_pipeline_t g_audio_pipeline;

void audio_pipeline_init(audio_pipeline_t *pipeline)
{
    if (!pipeline) return;
    memset(pipeline, 0, sizeof(audio_pipeline_t));
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_init(&pipeline->agc);
    audio_ring_buffer_init(&pipeline->ring_buf, pipeline->ring_storage, AUDIO_RING_BUFFER_SIZE);
    pipeline->active = false;
    pipeline->buffering = false;
    pipeline->session_id = 0;
    pipeline->total_frames_decoded = 0;
    pipeline->total_samples_pushed = 0;
}

void audio_pipeline_start_session(audio_pipeline_t *pipeline, uint8_t session_id)
{
    if (!pipeline) return;
    adpcm_init_state(&pipeline->adpcm);
    audio_filter_init(&pipeline->filter);
    audio_agc_reset(&pipeline->agc);
    audio_ring_buffer_clear(&pipeline->ring_buf);
    pipeline->session_id = session_id;
    pipeline->active = true;
    pipeline->buffering = true; // Wait for the prefill cushion to prevent jitter underruns
    pipeline->lead_mute_remaining = AUDIO_LEAD_MUTE_SAMPLES;
    pipeline->fade_in_remaining = AUDIO_FADE_IN_SAMPLES;
    pipeline->total_frames_decoded = 0;
    pipeline->total_samples_pushed = 0;
    pipeline->underrun_count = 0;
    pipeline->session_peak_in = 0;
    pipeline->session_peak_out = 0;
    pipeline->usb_reads = 0;
    pipeline->usb_padded = 0;
    pipeline->session_start_us = (uint64_t)esp_timer_get_time();
}

void audio_pipeline_sync(audio_pipeline_t *pipeline, int16_t predictor, int8_t step_index)
{
    if (!pipeline) return;
    adpcm_sync_state(&pipeline->adpcm, predictor, step_index);
}

void audio_pipeline_set_nibble_order(audio_pipeline_t *pipeline, bool low_nibble_first)
{
    if (!pipeline) return;
    adpcm_set_nibble_order(&pipeline->adpcm, low_nibble_first);
}

size_t audio_pipeline_feed_adpcm(audio_pipeline_t *pipeline, const uint8_t *adpcm_bytes, size_t len)
{
    if (!pipeline || !adpcm_bytes || len == 0 || !pipeline->active) return 0;

    // A BLE notification may carry more than one ATVV frame. Decode in bounded
    // chunks so temp_pcm can never overflow, and so the decoder state stays in
    // lock-step with the remote's frame boundaries (128 encoded bytes = 256
    // PCM samples per ATVV frame).
    size_t total_written = 0;
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > AUDIO_MAX_CHUNK_BYTES) {
            chunk = AUDIO_MAX_CHUNK_BYTES;
        }

        size_t samples_decoded = adpcm_decode_frame(&pipeline->adpcm,
                                                    adpcm_bytes + offset,
                                                    chunk,
                                                    pipeline->temp_pcm);
        offset += chunk;
        if (samples_decoded == 0) continue;

        // 1. (removed) Declip.
        //
        // audio_filter_declip() was a same-side-neighbour spike replacer with no
        // absolute-magnitude guard: its gate fires whenever |step| exceeds the
        // threshold in both directions and the neighbours sit closer to each
        // other than to the sample, which for a signal of any real amplitude is
        // "nearly always". Measured on clean tones it rewrote 12.5-50% of
        // samples, and at exactly fs/4 (2 kHz at 8 kHz) it is degenerate - the
        // peak samples land on [0, A, 0, -A], so prev == next and the
        // replacement equals them - nulling the tone to -36 dB at every
        // amplitude. On speech it costs dB across the band and injects energy
        // where there was none. The stream is IMA-ADPCM, which cannot contain
        // impulsive spikes in the first place, so there was nothing to repair.
        //
        // 2. (removed) 3-tap low-pass [0.25, 0.5, 0.25].
        //
        // Its response is cos^2(pi*f/fs): -3 dB at 1454 Hz, -10 dB at 2.5 kHz,
        // -25 dB at 3.4 kHz and a hard zero at 4 kHz. At an 8 kHz sample rate
        // speech carries its consonants in 2-4 kHz, so the filter deleted
        // exactly the band that carries the most recognition information. It
        // also cannot reduce noise: an 8 kHz stream is already band-limited to
        // 4 kHz, so codec and microphone noise share the speech band and no
        // in-band filter can separate them.
        //
        // If hiss ever needs suppressing it has to be a real FIR with a cutoff
        // near 3.6 kHz, not a 3-tap. A one-pole "y += (x - y) >> 3" is NOT a
        // mild filter: with a = 1/8 its corner is about 159 Hz at 8 kHz, which
        // would remove nearly all of the speech.

        // 3. Speech high-pass, two cascaded one-pole sections (12 dB/octave) at
        //    AUDIO_HP_HZ. Replaces a DC blocker whose corner was 19 Hz: the
        //    measured noise floor of a live session is 85% below 500 Hz, so a
        //    DC blocker removed none of what is actually audible as rumble.
        audio_filter_highpass(&pipeline->filter, pipeline->temp_pcm, samples_decoded);

        // 4. Dynamic AGC + soft clip. During the lead-mute window feed zeros so the
        //    peak envelope does not decay and the gain stays locked at 1.0x.
        for (size_t i = 0; i < samples_decoded; i++) {
            if (pipeline->lead_mute_remaining > 0) {
                pipeline->temp_pcm[i] = 0;
                pipeline->lead_mute_remaining--;
            }
        }
        for (size_t i = 0; i < samples_decoded; i++) {
            int32_t a = pipeline->temp_pcm[i];
            if (a < 0) a = -a;
            if (a > (int32_t)pipeline->session_peak_in) pipeline->session_peak_in = (uint16_t)a;
        }
        audio_agc_process(&pipeline->agc, pipeline->temp_pcm, samples_decoded);
        for (size_t i = 0; i < samples_decoded; i++) {
            int32_t a = pipeline->temp_pcm[i];
            if (a < 0) a = -a;
            if (a > (int32_t)pipeline->session_peak_out) pipeline->session_peak_out = (uint16_t)a;
        }

        // 5. Micro fade-in applied after the AGC to smooth the mute boundary.
        for (size_t i = 0; i < samples_decoded; i++) {
            if (pipeline->fade_in_remaining > 0) {
                uint32_t step = AUDIO_FADE_IN_SAMPLES - pipeline->fade_in_remaining;
                pipeline->temp_pcm[i] = (int16_t)(((int32_t)pipeline->temp_pcm[i] * (int32_t)step) / AUDIO_FADE_IN_SAMPLES);
                pipeline->fade_in_remaining--;
            }
        }

        // 6. Enqueue into the ring buffer
        size_t written = audio_ring_buffer_write(&pipeline->ring_buf, pipeline->temp_pcm, samples_decoded);
        total_written += written;

        pipeline->total_frames_decoded++;
        pipeline->total_samples_pushed += written;

        if (pipeline->buffering) {
            if (audio_ring_buffer_available_read(&pipeline->ring_buf) >= AUDIO_JITTER_PREFILL_SAMPLES) {
                pipeline->buffering = false;
            }
        }
    }

    return total_written;
}

size_t audio_pipeline_read_for_usb(audio_pipeline_t *pipeline, int16_t *out_pcm, size_t sample_count)
{
    if (!pipeline || !out_pcm || sample_count == 0) return 0;

    if (!pipeline->active || pipeline->buffering) {
        memset(out_pcm, 0, sample_count * sizeof(int16_t));
        return sample_count;
    }

    size_t avail = audio_ring_buffer_available_read(&pipeline->ring_buf);
    pipeline->usb_reads++;

    if (avail < sample_count) {
        pipeline->underrun_count++;
        pipeline->usb_padded += (uint32_t)(sample_count - avail);
        size_t n = audio_ring_buffer_read(&pipeline->ring_buf, out_pcm, avail);
        int16_t last_val = (n > 0) ? out_pcm[n - 1] : 0;
        for (size_t i = n; i < sample_count; i++) {
            last_val = (int16_t)((last_val * 7) / 8);
            out_pcm[i] = last_val;
        }
        return sample_count;
    }

    audio_ring_buffer_read(&pipeline->ring_buf, out_pcm, sample_count);
    return sample_count;
}

void audio_pipeline_stop_session(audio_pipeline_t *pipeline)
{
    if (!pipeline) return;

    // Session summary. Split deliberately into what the decoder produced
    // (peak_out, queued) and what USB actually consumed (usb_reads): a silent
    // microphone is either "no PCM was decoded" or "the host never opened the
    // stream", and those need opposite fixes. peak_out == 0 with queued > 0
    // means the samples were decoded but all zero; usb_reads == 0 means the
    // isochronous endpoint never ran, i.e. the host has not started capture.
    app_log("AUDIO", "Session: blocks=%lu queued=%lu peak_in=%u peak_out=%u usb_reads=%lu underrun=%lu still_buffering=%d",
            (unsigned long)pipeline->total_frames_decoded,
            (unsigned long)pipeline->total_samples_pushed,
            (unsigned)pipeline->session_peak_in, (unsigned)pipeline->session_peak_out,
            (unsigned long)pipeline->usb_reads, (unsigned long)pipeline->underrun_count,
            pipeline->buffering ? 1 : 0);

    pipeline->active = false;
    pipeline->buffering = false;
    pipeline->lead_mute_remaining = 0;
    pipeline->fade_in_remaining = 0;
    audio_ring_buffer_clear(&pipeline->ring_buf);
}
