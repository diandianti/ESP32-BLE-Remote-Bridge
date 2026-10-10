/*
 * MI-RC003-ESP32-Bridge - ATVV audio frame assembly
 *
 * Google's ATV Voice (ATVV) specification defines the microphone stream as a
 * sequence of 134-byte audio frames:
 *
 *   byte 0    version (0x02)
 *   byte 1    frame number (8-bit, wraps)
 *   byte 2    codec / sample rate, bit 2 = 16 kHz, bit 1 = 8 kHz
 *   byte 3    reserved
 *   byte 4-5  ADPCM sync word: predictor (16-bit LE) + step index (8-bit)
 *   byte 6.. 128 bytes of IMA/DVI ADPCM (4:1, two samples per byte)
 *
 * "When transmitted over the air an audio frame is split into 20-byte
 * packets", so one frame arrives as roughly seven notifications on the audio
 * characteristic. This module stitches those fragments back into whole frames
 * and hands the 128-byte payload to the PCM pipeline.
 *
 * The previous code fed each notification straight into the decoder, which
 * decoded the six header bytes as audio and lost sample alignment: that is
 * why the microphone produced noise even when the stream itself was healthy.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of encoded audio in one ATVV frame. */
#define ATVV_AUDIO_PAYLOAD_BYTES 128
/** Bytes of header in front of the payload. */
#define ATVV_AUDIO_HEADER_BYTES  6
/** Total size of one ATVV audio frame. */
#define ATVV_AUDIO_FRAME_BYTES   (ATVV_AUDIO_HEADER_BYTES + ATVV_AUDIO_PAYLOAD_BYTES)

/** Statistic snapshot for the device log. */
typedef struct {
    uint32_t packets;           // audio notifications accepted
    uint32_t frames;            // whole frames assembled
    uint32_t dropped_packets;   // fragments discarded while resynchronising
    uint32_t resyncs;           // times the frame boundary was re-acquired
    uint8_t  version;           // last header version byte
    uint8_t  last_frame_number; // last header frame number
    uint8_t  codec_bits;        // last header codec / sample-rate byte
    bool     sync_seen;         // header carried a non-zero sync word
    int16_t  last_predictor;    // last sync-word predictor
    int8_t   last_step_index;   // last sync-word step index
} atvv_audio_stats_t;

/** Reset the assembler and its statistics. */
void atvv_audio_reset(void);

/** @brief Restart assembly at a microphone-session boundary. */
void atvv_audio_begin_session(void);

/**
 * @brief Feed one audio-characteristic notification.
 *
 * Non-conforming input (the Xiaomi RC003 sends bare IMA-ADPCM with no ATVV
 * header) is passed through untouched so the existing decode path keeps
 * working. The frame assembler only engages once a header is recognised.
 *
 * @param data  Notification payload.
 * @param len   Payload length.
 */
void atvv_audio_feed(const uint8_t *data, size_t len);

/**
 * @brief Sink for decoded payload blocks.
 *
 * Implemented by the BLE layer: it forwards the bytes to the PCM pipeline.
 * Called with 128 bytes per conforming ATVV frame, or with the raw
 * notification for a bare-payload remote.
 */
void atvv_audio_payload_ready(const uint8_t *payload, size_t len);

/** @brief Copy the current statistics. */
void atvv_audio_get_stats(atvv_audio_stats_t *out_stats);

/**
 * @brief Sink for a frame's ADPCM sync word.
 *
 * Implemented by the BLE layer. Called before the frame's payload is handed to
 * atvv_audio_payload_ready(), so the decoder starts every frame from the
 * encoder's own predictor and step index.
 */
void atvv_audio_sync_ready(int16_t predictor, int8_t step_index);

/** @brief True once at least one valid ATVV header has been seen. */
bool atvv_audio_header_detected(void);

/** @brief True when the stream uses the sequence-numbered frame header. */
bool atvv_audio_seq_framed(void);

#ifdef __cplusplus
}
#endif
