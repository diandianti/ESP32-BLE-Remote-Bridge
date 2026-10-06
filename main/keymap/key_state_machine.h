#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "key_definitions.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTION_NONE = 0,
    ACTION_KEYBOARD_TAP,        // Tap a key or combo (down + up)
    ACTION_KEYBOARD_HOLD,       // Press and hold
    ACTION_KEYBOARD_RELEASE,    // Release key
    ACTION_CONSUMER_TAP,        // Tap a consumer control usage
    ACTION_CONSUMER_HOLD,
    ACTION_CONSUMER_RELEASE,
    ACTION_VOICE_HOLD,          // Trigger voice recording + hold hotkey
    ACTION_VOICE_RELEASE,       // End voice recording + release hotkey
    ACTION_SWITCH_LAYER,        // Switch layer (auto-toggles to 0 if already active)
    ACTION_TRANSPARENT,         // Inherit from Layer 0
    ACTION_MOUSE_BUTTON_TAP,    // Tap a mouse button (down + up)
    ACTION_MOUSE_BUTTON_HOLD,   // Press and hold a mouse button
    ACTION_MOUSE_BUTTON_RELEASE,// Internal: release a held mouse button on key-up
    ACTION_MOUSE_MOVE,          // Relative cursor move (dx, dy)
    ACTION_MOUSE_WHEEL,         // Relative wheel scroll
    ACTION_ENTER_SWITCH_MODE    // Enter the configuration switch mode (modal)
} key_action_type_t;

typedef struct {
    key_action_type_t type;
    uint8_t           modifier;
    uint8_t           key_code;      // Keyboard HID usage or mouse button mask
    uint16_t          consumer_code;
    uint8_t           target_layer;
    int8_t            mouse_dx;      // Mouse move X delta
    int8_t            mouse_dy;      // Mouse move Y delta
    int8_t            mouse_wheel;   // Wheel delta
} key_action_t;

typedef struct {
    uint8_t       source_vk;
    bool          has_click;
    key_action_t  click_action;
    bool          has_long;
    key_action_t  long_action;
    uint16_t      long_ms;
    bool          has_double;
    key_action_t  double_action;
    uint16_t      double_ms;
    bool          has_repeat;
    key_action_t  repeat_action;
    uint16_t      repeat_delay_ms;
    uint16_t      repeat_interval_ms;
} key_binding_t;

typedef struct {
    bool     is_pressed;
    uint32_t press_timestamp;
    uint32_t release_timestamp;
    uint8_t  press_count;
    bool     long_fired;
    uint32_t next_repeat_timestamp;
    bool     waiting_double;
    bool     move_active;            // Continuous mouse movement while held
    uint32_t next_move_timestamp;
} key_slot_state_t;

typedef struct {
    uint8_t  source_vk;
    bool     is_pressed;
    uint32_t timestamp;
    uint32_t duration_ms;
    uint8_t  action_type;
    uint8_t  modifier;
    uint8_t  key_code;
    uint16_t consumer_code;
    uint8_t  active_layer;
} key_event_telemetry_t;

// Upper bound on bindings per layer, and on physical key slots. The Google TV
// Remote can report button indices 1..16 (power, assistant, four D-pad
// directions, select, back, home, menu, volume +/-, input, mute and two
// preset-app buttons), and the slot used for an index is index-1, so this must
// stay above 16. get_physical_key_slot() rejects anything at or beyond this
// bound, which would otherwise silently drop the higher-index buttons.
#define MAX_KEY_BINDINGS   24
#define MAX_LAYERS         5
#define MAX_LAYER_NAME_LEN 24
#define MAX_SWITCH_MAP     16

// One entry of the global configuration-switch map: pressing the physical key
// `source_vk` while the switch mode is active selects `target_layer`. One key
// maps to exactly one configuration, but several keys may target the same one.
typedef struct {
    uint8_t source_vk;
    uint8_t target_layer;
} key_switch_map_entry_t;

typedef enum {
    LAYER_TYPE_PERSISTENT = 0,
    LAYER_TYPE_ONESHOT    = 1,
    LAYER_TYPE_TIMEOUT    = 2
} layer_type_t;

typedef struct {
    char          name[MAX_LAYER_NAME_LEN];
    layer_type_t  type;
    uint16_t      timeout_sec;
    uint32_t      led_color;
    key_binding_t bindings[MAX_KEY_BINDINGS];
    size_t        binding_count;
} key_layer_t;

typedef void (*key_output_callback_t)(const key_action_t *action);

typedef struct {
    key_layer_t           layers[MAX_LAYERS];
    size_t                layer_count;
    uint8_t               active_layer;
    uint32_t              last_activity_time;
    key_slot_state_t      states[MAX_KEY_BINDINGS];
    key_output_callback_t output_cb;
    key_event_telemetry_t last_telemetry;
    // Global configuration-switch map (see ACTION_ENTER_SWITCH_MODE).
    key_switch_map_entry_t switch_map[MAX_SWITCH_MAP];
    size_t                 switch_map_count;
    bool                   switch_mode_active;
    uint32_t               switch_mode_enter_ms;
    uint32_t               switch_mode_last_activity_ms;
    bool                   switch_mode_via_tv_rapid;
    // Bumped whenever the stored configuration (layers/bindings/switch map)
    // changes, so clients can detect device-side updates.
    uint32_t               config_rev;
} key_mapper_engine_t;

void key_engine_init(key_mapper_engine_t *engine, key_output_callback_t cb);
void key_engine_load_defaults(key_mapper_engine_t *engine);
void key_engine_switch_layer(key_mapper_engine_t *engine, uint8_t target_layer, uint32_t now_ms);
uint8_t key_engine_get_active_layer(const key_mapper_engine_t *engine);

/** @brief Enter the modal configuration-switch mode (LED breathes). */
void key_engine_enter_switch_mode(key_mapper_engine_t *engine, uint32_t now_ms, bool via_tv_rapid);

/** @brief Leave the configuration-switch mode without switching. */
void key_engine_exit_switch_mode(key_mapper_engine_t *engine);

/** @brief True while the configuration-switch mode is active. */
bool key_engine_switch_mode_active(const key_mapper_engine_t *engine);
bool key_engine_set_layer_binding(key_mapper_engine_t *engine, uint8_t layer_idx, const key_binding_t *binding);
bool key_engine_get_layer_binding(const key_mapper_engine_t *engine, uint8_t layer_idx, uint8_t source_vk, key_binding_t *out_binding);
bool key_engine_set_binding(key_mapper_engine_t *engine, const key_binding_t *binding);
bool key_engine_get_binding(const key_mapper_engine_t *engine, uint8_t source_vk, key_binding_t *out_binding);
void key_engine_feed_key(key_mapper_engine_t *engine, uint8_t raw_key_code, bool is_pressed, uint32_t now_ms);
void key_engine_tick(key_mapper_engine_t *engine, uint32_t now_ms);
void key_engine_release_all(key_mapper_engine_t *engine, uint32_t now_ms);

/** @brief Return the canonical code of a currently pressed key, or 0 if none. */
uint8_t key_engine_get_pressed_vk(const key_mapper_engine_t *engine);

/**
 * @brief Number of physical key slots.
 *
 * This is the length of the slot numbering every other keymap concept uses: a
 * binding's slot, a key name's slot and the index a remote reports (index - 1)
 * all refer to the same sequence.
 */
int key_engine_slot_count(void);

/** @brief Canonical key code for a physical slot, or 0 when out of range. */
uint8_t key_engine_vk_for_slot(int slot);

/**
 * @brief Physical slot for a key code, or -1 when the code is not a button.
 *
 * Accepts both the canonical codes the keymap stores and the raw codes a
 * remote reports, so callers do not have to know which one they are holding.
 */
int key_engine_slot_for_vk(uint8_t vk);

/** @brief Take/release the engine's recursive mutex (for bulk layer updates). */
void key_engine_lock(void);
void key_engine_unlock(void);

#ifdef __cplusplus
}
#endif
