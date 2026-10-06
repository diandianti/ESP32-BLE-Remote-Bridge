/*
 * MI-RC003-ESP32-Bridge - ATVV audio frame assembly
 *
 * See atvv_audio.h for the frame layout.
 *
 * Two stream shapes have to be supported, and they are distinguished by
 * looking at the actual bytes rather than by guessing from the remote's name:
 *
 *  - Conforming ATVV (Google TV Remote): notifications start with the 0x02
 *    version byte and carry fragments of a 134-byte frame.
 *  - Bare IMA-ADPCM (Xiaomi RC003): notifications are raw encoded audio with
 *    no framing at all; the first byte is a free-running packet counter, not
 *    a version constant.
 *
 * The assembler therefore starts in a "sniffing" state: it buffers the first
 * fragment without emitting anything, and commits to the ATVV path once a
 * second fragment also starts with 0x02 and the reserved header byte is zero.
 * Anything else is flushed straight through to the PCM pipeline, so the
 * RC003's audio path is byte-for-byte unchanged.
 *
 * SPDX-License-Identifier: MIT
 */

#include "atvv_audio.h"

#include <string.h>

#include "app_log.h"

// Header version byte of a conforming ATVV audio frame.
#define ATVV_VERSION 0x02

// Reserved byte 3 of the header; zero in every observed capture. Used together
// with the version byte as the two-fragment framing test.
#define ATVV_RESERVED 0x00

// Sniffing states.
typedef enum {
    ATVV_SNIFF_FIRST = 0,   // nothing buffered yet
    ATVV_SNIFF_CANDIDATE,   // first fragment started with 0x02, awaiting proof
    ATVV_STREAM_ATVV,       // conforming ATVV framing confirmed
    ATVV_STREAM_BARE,       // bare payload stream, pass everything through
} atvv_stream_t;

static atvv_stream_t s_stream = ATVV_SNIFF_FIRST;
static uint8_t  s_buf[ATVV_AUDIO_FRAME_BYTES];
static size_t   s_buf_len = 0;
static int      s_packets_since_header = 0;
static atvv_audio_stats_t s_stats;

// Provided by the BLE layer: decodes one block of encoded audio.
extern void atvv_audio_payload_ready(const uint8_t *payload, size_t len);

// Give up on a partial frame after this many fragments without a header.
#define ATVV_MAX_FRAGMENTS 10

static void emit_payload(const uint8_t *payload, size_t len)
{
    if (len) atvv_audio_payload_ready(payload, len);
}

static void emit_whole_frame(const uint8_t *frame)
{
    s_stats.version = frame[0];
    s_stats.last_frame_number = frame[1];
    s_stats.codec_bits = frame[2];

    const uint8_t pred = frame[4];
    const uint8_t step = frame[5];
    if (pred != 0 || step != 0) {
        s_stats.sync_seen = true;
        s_stats.last_predictor = (int16_t)pred;
        s_stats.last_step_index = (int8_t)step;
    }
    s_stats.frames++;
    emit_payload(frame + ATVV_AUDIO_HEADER_BYTES, ATVV_AUDIO_PAYLOAD_BYTES);
}

void atvv_audio_reset(void)
{
    s_stream = ATVV_SNIFF_FIRST;
    s_buf_len = 0;
    s_packets_since_header = 0;
    memset(&s_stats, 0, sizeof(s_stats));
}

void atvv_audio_begin_session(void)
{
    // A new microphone session always restarts on a frame boundary, but keep
    // the sniffing state: the framing test needs to run again because the
    // remote can be swapped between sessions.
    s_buf_len = 0;
    s_packets_since_header = 0;
    if (s_stream == ATVV_STREAM_ATVV) s_stream = ATVV_SNIFF_CANDIDATE;
}

// Commit to the bare payload interpretation and replay the buffered bytes.
static void fall_back_to_bare(void)
{
    if (s_stream == ATVV_STREAM_BARE) return;
    s_stream = ATVV_STREAM_BARE;
    if (s_buf_len) {
        emit_payload(s_buf, s_buf_len);
        s_buf_len = 0;
    }
    s_packets_since_header = 0;
}

void atvv_audio_feed(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;

    s_stats.packets++;

    const bool starts_with_version = (data[0] == ATVV_VERSION);
    const bool looks_like_header =
        starts_with_version && len >= 4 && data[3] == ATVV_RESERVED;

    if (s_stream == ATVV_STREAM_BARE) {
        emit_payload(data, len);
        return;
    }

    if (s_stream == ATVV_SNIFF_FIRST) {
        if (looks_like_header) {
            // Hold this fragment back until the next one confirms the framing.
            s_stream = ATVV_SNIFF_CANDIDATE;
            memcpy(s_buf, data, len < sizeof(s_buf) ? len : sizeof(s_buf));
            s_buf_len = len < sizeof(s_buf) ? len : sizeof(s_buf);
            s_packets_since_header = 0;
            return;
        }
        // Not an ATVV header: this is a bare encoded frame on its own.
        s_stream = ATVV_STREAM_BARE;
        emit_payload(data, len);
        return;
    }

    if (s_stream == ATVV_SNIFF_CANDIDATE) {
        if (!looks_like_header) {
            // The earlier 0x02 was payload data, not a header.
            fall_back_to_bare();
            emit_payload(data, len);
            return;
        }
        // Confirmed: process the held fragment, then this one as a continuation.
        s_stream = ATVV_STREAM_ATVV;
        uint8_t held[ATVV_AUDIO_FRAME_BYTES];
        size_t held_len = s_buf_len;
        memcpy(held, s_buf, held_len);
        s_buf_len = 0;
        s_packets_since_header = 0;

        if (held_len >= ATVV_AUDIO_FRAME_BYTES) {
            emit_whole_frame(held);
        } else {
            memcpy(s_buf, held, held_len);
            s_buf_len = held_len;
        }
        // Fall through to append the current fragment below.
    }

    // ATVV_STREAM_ATVV (or the tail of the candidate path).
    if (starts_with_version) {
        if (s_buf_len != 0) {
            // A new header arrived before the previous frame completed.
            s_stats.resyncs++;
            s_stats.dropped_packets++;
            s_buf_len = 0;
        }
        s_packets_since_header = 0;
    } else {
        s_packets_since_header++;
        if (s_packets_since_header > ATVV_MAX_FRAGMENTS) {
            s_stats.resyncs++;
            s_stats.dropped_packets++;
            s_buf_len = 0;
            s_packets_since_header = 0;
            return;
        }
    }

    size_t offset = 0;
    while (offset < len) {
        if (s_buf_len >= ATVV_AUDIO_FRAME_BYTES) {
            s_stats.dropped_packets++;
            s_buf_len = 0;
        }
        size_t space = ATVV_AUDIO_FRAME_BYTES - s_buf_len;
        size_t chunk = len - offset;
        if (chunk > space) chunk = space;
        memcpy(&s_buf[s_buf_len], data + offset, chunk);
        s_buf_len += chunk;
        offset += chunk;

        if (s_buf_len == ATVV_AUDIO_FRAME_BYTES) {
            emit_whole_frame(s_buf);
            s_buf_len = 0;
            s_packets_since_header = 0;
        } else {
            // Frame not complete: the rest of this notification, if any, must
            // belong to the next frame, so stop here.
            break;
        }
    }
}

void atvv_audio_get_stats(atvv_audio_stats_t *out_stats)
{
    if (out_stats) *out_stats = s_stats;
}

bool atvv_audio_header_detected(void)
{
    return s_stream == ATVV_STREAM_ATVV;
}
