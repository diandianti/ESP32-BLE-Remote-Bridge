#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Audio & DSP Parameters
// ==========================================
// The Google TV Remote was measured sending one 20-byte ADPCM block per audio
// notification at ~200 notifications/s, i.e. about 4000 encoded bytes/s. That
// is exactly 8 kHz mono 16-bit at the specified 4:1 ADPCM compression
// (8000 * 16 / 4 / 8 = 4000 B/s), and the remote's AUDIO_START reported
// codec 1, which the ATVV specification defines as "ADPCM, 8 kHz/16-bit".
//
// The Xiaomi RC003 uses the same 20-byte block size for its bare-payload
// stream. One ADPCM block must be decoded as one unit or the decoder's
// predictor and step index drift out of step with the encoder, so the block
// size is also the decode chunk size.
//
// Rebuild with AUDIO_SAMPLE_RATE 16000 for a 16 kHz remote: the UAC descriptor
// and the isochronous packet size both follow this macro.
#define AUDIO_SAMPLE_RATE         8000      // 8 kHz sample rate
#define AUDIO_BITS_PER_SAMPLE     16        // 16-bit PCM
#define AUDIO_CHANNELS            1         // Mono
#define AUDIO_DEFAULT_FRAME_BYTES 20        // Encoded ADPCM bytes per block
#define AUDIO_DEFAULT_FRAME_SAMPS 40        // 20 * 2 = 40 PCM samples per block
#define AUDIO_RING_BUFFER_SIZE    8192      // Ring buffer capacity (in samples, ~1 s)

// AGC & Filter parameters
//
// The AGC runs as a near-neutral limiter on purpose. Its previous setting
// (floor 1000, max gain 4.0) meant that during silence the envelope collapsed
// to the floor and the gain sat pinned at 4.0 - a fixed +12 dB on the noise
// floor, which is heard as hiss between words and is worse for recognition than
// a lower, consistent level. The floor is now close to the target so the gain
// stays near unity when there is nothing to amplify, and it can only lift a
// genuinely quiet talker by about 1.6 dB.
#define AGC_TARGET_LEVEL          12000.0f  // Target peak (~-8.7 dBFS)
#define AGC_DECAY_RATE            0.9997f   // Peak envelope decay per sample (~330 ms release)
#define AGC_MAX_GAIN              1.2f      // Cap boost at +1.6 dB: never chase the noise floor
#define AGC_NOISE_FLOOR           10000.0f  // Near the target, so silence passes at ~unity gain
#define AGC_MIN_GAIN              0.35f     // Lower clamp: never duck a loud syllable into nothing

// Lead-in mute. This window is *transmitted* digital silence, and the session is
// armed by the voice-key press rather than by the first audio, so every
// millisecond here is a millisecond of the user's first word that never reaches
// the host. 150 ms was long enough to eat a whole syllable; 20 ms still hides
// the decoder's start-up transient.
#define AUDIO_LEAD_MUTE_SAMPLES   (AUDIO_SAMPLE_RATE * 20 / 1000)
#define AUDIO_FADE_IN_SAMPLES     (AUDIO_SAMPLE_RATE * 4 / 1000)  // 4 ms, just de-clicks
#define DECLIP_THRESHOLD          1000

// Speech high-pass corner. Applied as two cascaded one-pole sections, so the
// roll-off is 12 dB/octave: -4.6 dB at 120 Hz, -11.5 dB at 60 Hz, -21.7 dB at
// 30 Hz. Chosen because the measured noise floor of a live session is 85%
// concentrated below 500 Hz, while every consonant sits above 1 kHz - so this
// removes the rumble without touching intelligibility. Lower it if a deep voice
// starts sounding thin: the fundamental of a male voice is 85-180 Hz.
#define AUDIO_HP_HZ               100.0f

// ==========================================
// 2. BLE Central & ATVV Parameters
// ==========================================
// Name substrings that identify a supported voice remote during a scan. The
// Google TV Remote advertises as "Google TV Remote"; the "Remote" entry alone
// already matches it, the explicit entries keep the intent obvious.
#define BLE_REMOTE_NAME_PREFIX    "MI RC"
#define BLE_REMOTE_NAME_EXTRA_1   "Google TV"
#define BLE_REMOTE_NAME_EXTRA_2   "Google"
#define BLE_SCAN_INTERVAL_MS      100
#define BLE_SCAN_WINDOW_MS        80
#define BLE_KEEP_ALIVE_INTERVAL   2000      // MIC_EXTEND interval during active voice (ms)
#define BLE_MAX_DISCOVERED        32        // Discovered device cache size

// ATVV GATT UUIDs (128-bit)
#define ATVV_SVC_UUID             "ab5e0001-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CMD_UUID        "ab5e0002-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_AUD_UUID        "ab5e0003-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CTL_UUID        "ab5e0004-5a21-4f05-bc7d-af01f617b664"

// Standard HOGP service / characteristics (16-bit)
#define HOGP_SVC_UUID             0x1812
#define HOGP_REPORT_CHAR_UUID     0x2A4D
#define HOGP_PROTOCOL_MODE_UUID   0x2A4E
#define HOGP_CONTROL_POINT_UUID   0x2A4C
#define HOGP_CCCD_UUID            0x2902

// ==========================================
// 3. FreeRTOS Task & Multi-Core Pinning
// ==========================================
#define TASK_CORE_BLE             0         // Core 0: NimBLE host & audio decoding
#define TASK_CORE_USB             1         // Core 1: TinyUSB & HID/audio push

#define PRIO_TASK_USB             6
#define PRIO_TASK_AUDIO_DSP       5
#define PRIO_TASK_BLE             4
#define PRIO_TASK_KEYMAP_CLI      3

// ==========================================
// 4. Default Keymap & Hotkey Codes
// ==========================================
// Default voice input hotkey: Right Alt + Comma (WeChat voice IME).
// HID keyboard modifiers: 0x01=LCTRL, 0x02=LSHIFT, 0x04=LALT, 0x08=LGUI,
//                         0x10=RCTRL, 0x20=RSHIFT, 0x40=RALT, 0x80=RGUI
#define DEFAULT_VOICE_MODIFIER    0x40      // KEY_MOD_RALT
#define DEFAULT_VOICE_KEY         0x36      // HID usage for ',' 

// ==========================================
// 5. USB composite device layout
// ==========================================
// Interface numbers (must match usb_descriptors.c)
#define USB_ITF_UAC_AC            0
#define USB_ITF_UAC_AS            1
#define USB_ITF_HID               2
#define USB_ITF_VENDOR            3

// Endpoint addresses
// NOTE: the ESP32-S3 DWC2 exposes very few IN endpoints (effectively EP1-EP4).
// Keep every IN endpoint within that range.
#define USB_EP_UAC_IN             0x81      // Isochronous IN  (microphone)
#define USB_EP_HID_IN             0x82      // Interrupt IN    (keyboard/consumer)
#define USB_EP_VENDOR_IN          0x83      // Bulk IN         (WebUSB responses)
#define USB_EP_VENDOR_OUT         0x05      // Bulk OUT        (WebUSB requests)

// Bytes per isochronous transfer: 2 ms of mono 16-bit audio at the configured
// sample rate (64 at 16 kHz, 32 at 8 kHz). The host validates this against the
// declared sample rate and rejects the device with "Error 43" if it does not
// match, so it must be derived from AUDIO_SAMPLE_RATE rather than hard-coded.
#define USB_UAC_PACKET_BYTES      (AUDIO_SAMPLE_RATE * 2 * AUDIO_BITS_PER_SAMPLE / 8 / 1000)
#define USB_UAC_PACKET_SAMPLES    (USB_UAC_PACKET_BYTES / 2)

// WebUSB bulk transfer / framing
#define WEBUSB_FRAME_SOF0         0x4D      // 'M'
#define WEBUSB_FRAME_SOF1         0x52      // 'R'
#define WEBUSB_FRAME_HEADER_LEN   6
#define WEBUSB_MAX_PAYLOAD        32768     // full multi-layer keymap JSON is a few KB
#define WEBUSB_TX_CHUNK           64

// ==========================================
// 6. On-board RGB LED
// ==========================================
#ifndef RGB_BUILTIN
#define RGB_BUILTIN               48        // GPIO48 on most ESP32-S3 DevKitC-1 boards
#endif

#ifdef __cplusplus
}
#endif
