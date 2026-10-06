#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Xiaomi Remote Physical Key Codes
//    (canonical internal codes; HID usages and their alternates are normalized
//     to these by key_state_machine.c)
// ==========================================
#define MI_KEY_VOL_UP       0x80    // Volume Up
#define MI_KEY_VOL_DOWN     0x81    // Volume Down
#define MI_KEY_BACK         0xF1    // Back key
#define MI_KEY_POWER        0x66    // Power button
#define MI_KEY_POWER_ALT    0xFF
#define MI_KEY_HOME         0x24    // Home button
#define MI_KEY_HOME_ALT     0x4A    // HID usage 0x4A (RC003 HOGP report)
#define MI_KEY_MENU         0x5D    // Menu button
#define MI_KEY_MENU_ALT     0x65    // HID usage 0x65
#define MI_KEY_TV           0xC0    // TV button
#define MI_KEY_TV_ALT       0x35    // HID usage 0x35
#define MI_KEY_UP           0x52    // D-Pad Up
#define MI_KEY_DOWN         0x51    // D-Pad Down
#define MI_KEY_LEFT         0x50    // D-Pad Left
#define MI_KEY_RIGHT        0x4F    // D-Pad Right
#define MI_KEY_OK           0x28    // OK / Enter
#define MI_KEY_VOICE        0x04    // Voice button (ATVV)
#define MI_KEY_VOICE_ALT    0x3E    // Voice button (HID usage F5)

// ==========================================
// 1b. Google TV Remote (G20BTS / G9N9N) key codes
//
// The Google TV Remote does not send HID keyboard usages. Its HOGP input
// report is a 1-byte button index delivered as `[index 00]` (2 bytes) and
// occasionally `[index 00 00 00 00 00]` (6 bytes), with an all-zero report on
// release. The index is positional and follows the standard Google TV remote
// button list, in the same order as PHYSICAL_KEYS in the WebUSB config site, so
// the canonical codes below are also the source_vk values used by the key map:
//
//   index 1  Power          | 2  Google Assistant (mic) | 3  D-Pad Up
//   index 4  D-Pad Down     | 5  D-Pad Left             | 6  D-Pad Right
//   index 7  Select (OK)    | 8  Back                   | 9  Home
//   index 10 Menu           | 11 Volume +               | 12 Volume -
//   index 13 Input (source select)
//
// Every one of these indices was confirmed against a real remote: each was
// observed in the device log together with the key it produced.
//
// Indices 14..16 are reserved for the mute and preset-app buttons (YouTube /
// Netflix and similar) that some variants carry. They stay unmapped until a
// unit reports them, and an unrecognised index is logged with its raw bytes
// rather than being guessed at.
// ==========================================
#define MI_KEY_GTV_POWER        0x70
#define MI_KEY_GTV_VOICE        0x71
#define MI_KEY_GTV_UP           0x72
#define MI_KEY_GTV_DOWN         0x73
#define MI_KEY_GTV_LEFT         0x74
#define MI_KEY_GTV_RIGHT        0x75
#define MI_KEY_GTV_OK           0x76
#define MI_KEY_GTV_BACK         0x77
#define MI_KEY_GTV_HOME         0x78
#define MI_KEY_GTV_MENU         0x79
#define MI_KEY_GTV_VOL_UP       0x7A
#define MI_KEY_GTV_VOL_DOWN     0x7B
#define MI_KEY_GTV_INPUT        0x7C
#define MI_KEY_GTV_MUTE         0x7D
#define MI_KEY_GTV_APP_1        0x7E
#define MI_KEY_GTV_APP_2        0x7F

// Index 13 is the input-source key on the standard button list. The earlier
// name called it the TV key, which is why it used to send F8.
#define MI_KEY_GTV_TV           MI_KEY_GTV_INPUT

// Highest button index the Google TV Remote layout can report.
#define MI_KEY_GTV_INDEX_MAX    16

// ==========================================
// 2. USB HID Keyboard Modifier Bitmasks
// ==========================================
#define USB_MOD_NONE        0x00
#define USB_MOD_LCTRL       0x01
#define USB_MOD_LSHIFT      0x02
#define USB_MOD_LALT        0x04
#define USB_MOD_LGUI        0x08
#define USB_MOD_RCTRL       0x10
#define USB_MOD_RSHIFT      0x20
#define USB_MOD_RALT        0x40
#define USB_MOD_RGUI        0x80

// ==========================================
// 3. Standard USB HID Keyboard Key Codes
// ==========================================
#define USB_KEY_NONE        0x00
#define USB_KEY_A           0x04
#define USB_KEY_D           0x07    // 'D' (Win+D show desktop)
#define USB_KEY_H           0x0B    // 'H' (Win+H voice typing)
#define USB_KEY_N           0x11    // 'N' (Win+N notification centre, Netflix)
#define USB_KEY_RETURN      0x28    // Enter
#define USB_KEY_ESCAPE      0x29    // Esc
#define USB_KEY_BACKSPACE   0x2A
#define USB_KEY_TAB         0x2B    // Tab (Alt+Tab task switch)
#define USB_KEY_SPACE       0x2C    // Spacebar
#define USB_KEY_F5          0x3E    // F5 refresh
#define USB_KEY_F8          0x41    // F8 voice IME hotkey
#define USB_KEY_RIGHT       0x4F    // Arrow right
#define USB_KEY_LEFT        0x50    // Arrow left
#define USB_KEY_DOWN        0x51    // Arrow down
#define USB_KEY_UP          0x52    // Arrow up
#define USB_KEY_COMMA       0x36    // ',' (RAlt+, WeChat IME hotkey)

// ==========================================
// 4. USB HID Consumer Control Usages
// ==========================================
#define USB_CONSUMER_NONE           0x0000
#define USB_CONSUMER_POWER          0x0030
#define USB_CONSUMER_SLEEP          0x0032
#define USB_CONSUMER_PLAY_PAUSE     0x00CD
#define USB_CONSUMER_MUTE           0x00E2
#define USB_CONSUMER_VOLUME_UP      0x00E9
#define USB_CONSUMER_VOLUME_DOWN    0x00EA
#define USB_CONSUMER_NEXT_TRACK     0x00B5
#define USB_CONSUMER_PREV_TRACK     0x00B6
#define USB_CONSUMER_AC_HOME        0x0223
#define USB_CONSUMER_AC_BACK        0x0224
#define USB_CONSUMER_AC_SEARCH      0x0221  // search / source select

// ==========================================
// 5. USB HID Mouse Button Bitmasks
// ==========================================
#define USB_MOUSE_BTN_LEFT      0x01    // Left button
#define USB_MOUSE_BTN_RIGHT     0x02    // Right button
#define USB_MOUSE_BTN_MIDDLE    0x04    // Middle button
#define USB_MOUSE_BTN_BACK      0x08    // Back (side button)
#define USB_MOUSE_BTN_FORWARD   0x10    // Forward (side button)

#ifdef __cplusplus
}
#endif
