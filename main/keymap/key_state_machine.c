#include "key_state_machine.h"
#include "app_config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_key_engine_mutex = NULL;

// While a mouse-move key is held, emit one relative step every this many ms.
#define MOUSE_MOVE_INTERVAL_MS 15

// Configuration-switch mode auto-exits after this much inactivity.
#define SWITCH_MODE_TIMEOUT_MS 5000

// After entering via the rapid-TV shortcut, ignore TV for this long so the
// tail of the rapid sequence cannot immediately exit the mode.
#define SWITCH_MODE_TV_LOCKOUT_MS 2000

// Rapid TV presses that force the switch mode even when the active
// configuration has no switch key bound.
#define TV_RAPID_PRESS_COUNT   5
#define TV_RAPID_WINDOW_MS     1500

// Latest engine time, kept so action handlers can timestamp events without
// threading now_ms through every emit_action() call site.
static uint32_t s_now_ms = 0;

// Rapid TV-press detector for the fallback shortcut.
static uint8_t  s_tv_rapid_count = 0;
static uint32_t s_tv_rapid_first_ms = 0;

extern void led_indicator_set_switch_mode(bool active);

void key_engine_lock(void)
{
    if (!s_key_engine_mutex) {
        s_key_engine_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_key_engine_mutex) {
        xSemaphoreTakeRecursive(s_key_engine_mutex, portMAX_DELAY);
    }
}

void key_engine_unlock(void)
{
    if (s_key_engine_mutex) {
        xSemaphoreGiveRecursive(s_key_engine_mutex);
    }
}

extern void app_log(const char *tag, const char *format, ...);
extern void led_indicator_set_layer_color(uint32_t rgb_color);

static int get_physical_key_slot(uint8_t raw_key)
{
    switch (raw_key) {
        case MI_KEY_POWER: case MI_KEY_POWER_ALT: return 0;
        case MI_KEY_VOICE: case MI_KEY_VOICE_ALT: return 1;
        case MI_KEY_UP:    return 2;
        case MI_KEY_DOWN:  return 3;
        case MI_KEY_LEFT:  return 4;
        case MI_KEY_RIGHT: return 5;
        case MI_KEY_OK:    return 6;
        case MI_KEY_BACK:  return 7;
        case MI_KEY_HOME: case MI_KEY_HOME_ALT: return 8;
        case MI_KEY_MENU: case MI_KEY_MENU_ALT: return 9;
        case MI_KEY_VOL_UP:   return 10;
        case MI_KEY_VOL_DOWN: return 11;
        case MI_KEY_TV: case MI_KEY_TV_ALT: return 12;
        // Google TV Remote reports button indices natively, so the canonical
        // codes already carry the physical slot (index - 1).
        case MI_KEY_GTV_POWER:    return 0;
        case MI_KEY_GTV_VOICE:    return 1;
        case MI_KEY_GTV_UP:       return 2;
        case MI_KEY_GTV_DOWN:     return 3;
        case MI_KEY_GTV_LEFT:     return 4;
        case MI_KEY_GTV_RIGHT:    return 5;
        case MI_KEY_GTV_OK:       return 6;
        case MI_KEY_GTV_BACK:     return 7;
        case MI_KEY_GTV_HOME:     return 8;
        case MI_KEY_GTV_MENU:     return 9;
        case MI_KEY_GTV_VOL_UP:   return 10;
        case MI_KEY_GTV_VOL_DOWN: return 11;
        case MI_KEY_GTV_INPUT:    return 12;
        // Mute and the preset-app buttons sit past the 13-key layout, so they
        // get their own gesture slots rather than aliasing an existing key.
        case MI_KEY_GTV_MUTE:     return 13;
        case MI_KEY_GTV_APP_1:    return 14;
        case MI_KEY_GTV_APP_2:    return 15;
        default: return -1;
    }
}

static uint8_t canonical_source_vk(uint8_t raw_key)
{
    switch (raw_key) {
        case MI_KEY_POWER_ALT: return MI_KEY_POWER;
        case MI_KEY_VOICE_ALT: return MI_KEY_VOICE;
        case MI_KEY_HOME_ALT:  return MI_KEY_HOME;
        case MI_KEY_MENU_ALT:  return MI_KEY_MENU;
        case MI_KEY_TV_ALT:    return MI_KEY_TV;
        // Google TV indices arrive canonically; map them onto the shared
        // semantic codes so a single default key map covers both remotes.
        case MI_KEY_GTV_POWER:    return MI_KEY_POWER;
        case MI_KEY_GTV_VOICE:    return MI_KEY_VOICE;
        case MI_KEY_GTV_UP:       return MI_KEY_UP;
        case MI_KEY_GTV_DOWN:     return MI_KEY_DOWN;
        case MI_KEY_GTV_LEFT:     return MI_KEY_LEFT;
        case MI_KEY_GTV_RIGHT:    return MI_KEY_RIGHT;
        case MI_KEY_GTV_OK:       return MI_KEY_OK;
        case MI_KEY_GTV_BACK:     return MI_KEY_BACK;
        case MI_KEY_GTV_HOME:     return MI_KEY_HOME;
        case MI_KEY_GTV_MENU:     return MI_KEY_MENU;
        case MI_KEY_GTV_VOL_UP:   return MI_KEY_VOL_UP;
        case MI_KEY_GTV_VOL_DOWN: return MI_KEY_VOL_DOWN;
        case MI_KEY_GTV_INPUT:    return MI_KEY_TV;
        default: return raw_key;
    }
}

// Look up the target configuration for a key in the global switch map.
// Returns -1 when the key is not part of the map.
static int find_switch_target(const key_mapper_engine_t *engine, uint8_t can_vk)
{
    if (!engine) return -1;
    for (size_t i = 0; i < engine->switch_map_count; i++) {
        if (canonical_source_vk(engine->switch_map[i].source_vk) == can_vk) {
            return (int)engine->switch_map[i].target_layer;
        }
    }
    return -1;
}

static int find_binding_index_in_layer(const key_layer_t *layer, uint8_t raw_key)
{
    if (!layer) return -1;
    uint8_t can_vk = canonical_source_vk(raw_key);
    for (size_t i = 0; i < layer->binding_count; i++) {
        if (canonical_source_vk(layer->bindings[i].source_vk) == can_vk) {
            return (int)i;
        }
    }
    return -1;
}

// Resolve the binding for a key on the active configuration. Configurations
// 1..4 are independent from the default configuration: a key only acts if it
// is defined in the active configuration. Use the explicit "transparent"
// action (ACTION_TRANSPARENT) to inherit a gesture from the default config.
static void get_effective_binding(const key_mapper_engine_t *engine, uint8_t raw_key, key_binding_t *out_b)
{
    memset(out_b, 0, sizeof(key_binding_t));
    out_b->source_vk = canonical_source_vk(raw_key);

    uint8_t cur = engine->active_layer;

    if (cur == 0) {
        int idx0 = find_binding_index_in_layer(&engine->layers[0], raw_key);
        if (idx0 >= 0) {
            *out_b = engine->layers[0].bindings[idx0];
        }
        return;
    }
    if (cur >= MAX_LAYERS) {
        return;
    }

    // Start from the active configuration's own binding.
    const key_layer_t *layer = &engine->layers[cur];
    int idx = find_binding_index_in_layer(layer, raw_key);
    if (idx >= 0) {
        *out_b = layer->bindings[idx];
    }

    // Explicit transparency: fall back to the default config per gesture.
    int idx0 = find_binding_index_in_layer(&engine->layers[0], raw_key);
    if (idx0 < 0) {
        return;
    }
    const key_binding_t *base = &engine->layers[0].bindings[idx0];

    if (out_b->has_click && out_b->click_action.type == ACTION_TRANSPARENT) {
        out_b->click_action = base->click_action;
        out_b->has_click = base->has_click;
    }
    if (out_b->has_long && out_b->long_action.type == ACTION_TRANSPARENT) {
        out_b->long_action = base->long_action;
        out_b->long_ms = base->long_ms;
        out_b->has_long = base->has_long;
    }
    if (out_b->has_double && out_b->double_action.type == ACTION_TRANSPARENT) {
        out_b->double_action = base->double_action;
        out_b->double_ms = base->double_ms;
        out_b->has_double = base->has_double;
    }
    if (out_b->has_repeat && out_b->repeat_action.type == ACTION_TRANSPARENT) {
        out_b->repeat_action = base->repeat_action;
        out_b->repeat_delay_ms = base->repeat_delay_ms;
        out_b->repeat_interval_ms = base->repeat_interval_ms;
    }
}

static void emit_action(key_mapper_engine_t *engine, const key_action_t *action, uint8_t source_vk, bool is_down)
{
    if (!engine || !action || action->type == ACTION_NONE || action->type == ACTION_TRANSPARENT) return;

    engine->last_telemetry.source_vk = source_vk;
    engine->last_telemetry.is_pressed = is_down;
    engine->last_telemetry.action_type = action->type;
    engine->last_telemetry.modifier = action->modifier;
    engine->last_telemetry.key_code = action->key_code;
    engine->last_telemetry.consumer_code = action->consumer_code;
    engine->last_telemetry.active_layer = engine->active_layer;

    if (action->type == ACTION_SWITCH_LAYER) {
        uint8_t target = action->target_layer;
        uint8_t next_layer = (engine->active_layer == target) ? 0 : target;
        key_engine_switch_layer(engine, next_layer, engine->last_telemetry.timestamp);
        return;
    }

    if (action->type == ACTION_ENTER_SWITCH_MODE) {
        key_engine_enter_switch_mode(engine, s_now_ms, false);
        return;
    }

    if (engine->output_cb) {
        engine->output_cb(action);
    }

    if (engine->active_layer != 0 && engine->layers[engine->active_layer].type == LAYER_TYPE_ONESHOT) {
        if (action->type == ACTION_KEYBOARD_TAP || action->type == ACTION_CONSUMER_TAP ||
            action->type == ACTION_KEYBOARD_RELEASE || action->type == ACTION_CONSUMER_RELEASE ||
            action->type == ACTION_VOICE_RELEASE ||
            action->type == ACTION_MOUSE_BUTTON_TAP || action->type == ACTION_MOUSE_BUTTON_RELEASE ||
            action->type == ACTION_MOUSE_MOVE || action->type == ACTION_MOUSE_WHEEL) {
            key_engine_switch_layer(engine, 0, engine->last_telemetry.timestamp);
        }
    }
}

static void emit_action_as_tap_if_hold(key_mapper_engine_t *engine, const key_action_t *action, uint8_t source_vk)
{
    if (!action || action->type == ACTION_NONE || action->type == ACTION_TRANSPARENT) return;
    if (action->type == ACTION_KEYBOARD_HOLD) {
        key_action_t tap = { ACTION_KEYBOARD_TAP, action->modifier, action->key_code, 0, 0, 0, 0, 0 };
        emit_action(engine, &tap, source_vk, false);
    } else if (action->type == ACTION_CONSUMER_HOLD) {
        key_action_t tap = { ACTION_CONSUMER_TAP, 0, 0, action->consumer_code, 0, 0, 0, 0 };
        emit_action(engine, &tap, source_vk, false);
    } else if (action->type == ACTION_MOUSE_BUTTON_HOLD) {
        key_action_t tap = { ACTION_MOUSE_BUTTON_TAP, 0, action->key_code, 0, 0, 0, 0, 0 };
        emit_action(engine, &tap, source_vk, false);
    } else {
        emit_action(engine, action, source_vk, false);
    }
}

void key_engine_switch_layer(key_mapper_engine_t *engine, uint8_t target_layer, uint32_t now_ms)
{
    if (!engine) return;
    if (target_layer >= MAX_LAYERS) target_layer = 0;

    if (engine->switch_mode_active) {
        engine->switch_mode_active = false;
        led_indicator_set_switch_mode(false);
    }

    if (engine->active_layer == target_layer) return;

    key_engine_release_all(engine, now_ms);

    engine->active_layer = target_layer;
    engine->last_activity_time = now_ms;

    led_indicator_set_layer_color(engine->layers[target_layer].led_color);
    app_log("KEYMAP", "Layer Switched -> [%u: %s]", target_layer, engine->layers[target_layer].name);

    // The selection is intentionally NOT persisted. Which layer is active is
    // runtime state, and with the switch map bound to the D-pad, switching is a
    // routine keypress - persisting it meant a flash write per keypress. A reboot
    // starts on layer 0; only configuration changes ever reach flash.
}

void key_engine_enter_switch_mode(key_mapper_engine_t *engine, uint32_t now_ms, bool via_tv_rapid)
{
    if (!engine) return;
    key_engine_lock();
    engine->switch_mode_active = true;
    engine->switch_mode_enter_ms = now_ms;
    engine->switch_mode_last_activity_ms = now_ms;
    engine->switch_mode_via_tv_rapid = via_tv_rapid;
    // Drop any held-key state so releasing them after the mode exits does not
    // emit the underlying action.
    memset(engine->states, 0, sizeof(engine->states));
    uint8_t layer = engine->active_layer;
    key_engine_unlock();

    led_indicator_set_switch_mode(true);
    app_log("KEYMAP", "Config switch mode ON (layer %u)", layer);
}

void key_engine_exit_switch_mode(key_mapper_engine_t *engine)
{
    if (!engine) return;
    key_engine_lock();
    bool was_active = engine->switch_mode_active;
    engine->switch_mode_active = false;
    if (was_active) {
        memset(engine->states, 0, sizeof(engine->states));
    }
    key_engine_unlock();

    if (was_active) {
        led_indicator_set_switch_mode(false);
        app_log("KEYMAP", "Config switch mode OFF");
    }
}

bool key_engine_switch_mode_active(const key_mapper_engine_t *engine)
{
    if (!engine) return false;
    key_engine_lock();
    bool active = engine->switch_mode_active;
    key_engine_unlock();
    return active;
}

uint8_t key_engine_get_active_layer(const key_mapper_engine_t *engine)
{
    if (!engine) return 0;
    return engine->active_layer;
}

void key_engine_load_defaults(key_mapper_engine_t *engine)
{
    if (!engine) return;

    memset(engine->layers, 0, sizeof(engine->layers));
    engine->layer_count = MAX_LAYERS;
    engine->active_layer = 0;
    engine->last_activity_time = 0;
    engine->switch_mode_active = false;
    engine->switch_mode_enter_ms = 0;
    engine->switch_mode_last_activity_ms = 0;
    engine->switch_mode_via_tv_rapid = false;
    engine->config_rev++;

    // Default configuration-switch map: only the four directions are
    // user-editable; the confirm key is locked to the default configuration
    // in the engine.
    engine->switch_map_count = 0;
    engine->switch_map[engine->switch_map_count++] = (key_switch_map_entry_t){ MI_KEY_UP,    1 };
    engine->switch_map[engine->switch_map_count++] = (key_switch_map_entry_t){ MI_KEY_RIGHT, 2 };
    engine->switch_map[engine->switch_map_count++] = (key_switch_map_entry_t){ MI_KEY_DOWN,  3 };
    engine->switch_map[engine->switch_map_count++] = (key_switch_map_entry_t){ MI_KEY_LEFT,  4 };

    // ----------------------------------------------------
    // Layer 0: default
    // ----------------------------------------------------
    key_layer_t *l0 = &engine->layers[0];
    strncpy(l0->name, "默认配置", sizeof(l0->name) - 1);
    l0->type = LAYER_TYPE_PERSISTENT;
    l0->timeout_sec = 0;
    l0->led_color = 0x00FF00;
    l0->binding_count = 0;

    // Power: click Alt+Tab (task switch), long-press system sleep. A power
    // button has no host-side meaning for a USB keyboard, so the useful
    // interpretation is a window-switch on tap and sleep on hold.
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_POWER;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_TAP, USB_MOD_LALT, USB_KEY_TAB, 0, 0, 0, 0, 0 };
        b.has_long = true;
        b.long_ms = 600;
        b.long_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_SLEEP, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Voice: hold to stream microphone + hotkey
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_VOICE;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_VOICE_HOLD, DEFAULT_VOICE_MODIFIER, DEFAULT_VOICE_KEY, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // D-Pad
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_UP;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_HOLD, USB_MOD_NONE, USB_KEY_UP, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_DOWN;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_HOLD, USB_MOD_NONE, USB_KEY_DOWN, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_LEFT;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_HOLD, USB_MOD_NONE, USB_KEY_LEFT, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_RIGHT;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_HOLD, USB_MOD_NONE, USB_KEY_RIGHT, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // OK
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_OK;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_TAP, USB_MOD_NONE, USB_KEY_RETURN, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Back
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_BACK;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_AC_BACK, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Home -> Win+D
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_HOME;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_TAP, USB_MOD_LGUI, USB_KEY_D, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Menu -> Space
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_MENU;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_KEYBOARD_TAP, USB_MOD_NONE, USB_KEY_SPACE, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Volume up with repeat
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_VOL_UP;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_VOLUME_UP, 0, 0, 0, 0 };
        b.has_repeat = true;
        b.repeat_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_VOLUME_UP, 0, 0, 0, 0 };
        b.repeat_delay_ms = 350;
        b.repeat_interval_ms = 70;
        l0->bindings[l0->binding_count++] = b;
    }
    // Volume down with repeat
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_VOL_DOWN;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_VOLUME_DOWN, 0, 0, 0, 0 };
        b.has_repeat = true;
        b.repeat_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_VOLUME_DOWN, 0, 0, 0, 0 };
        b.repeat_delay_ms = 350;
        b.repeat_interval_ms = 70;
        l0->bindings[l0->binding_count++] = b;
    }
    // Input (source select): a search prompt is the closest host-side meaning,
    // so it opens the app/file search box. Long-press enters the configuration
    // switch mode, which is the only way to change layers without the web page.
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_TV;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_AC_SEARCH, 0, 0, 0, 0 };
        b.has_long = true;
        b.long_ms = 600;
        b.long_action = (key_action_t){ ACTION_ENTER_SWITCH_MODE, 0, 0, 0, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Mute, and the preset app buttons. Only present on some variants; a unit
    // that lacks them simply never reports these codes.
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_GTV_MUTE;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_MUTE, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    // Preset app 1 -> Windows notification centre is not useful; use the
    // browser-era convention of a plain media-play tap instead, which every
    // video site understands.
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_GTV_APP_1;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_PLAY_PAUSE, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }
    {
        key_binding_t b;
        memset(&b, 0, sizeof(b));
        b.source_vk = MI_KEY_GTV_APP_2;
        b.has_click = true;
        b.click_action = (key_action_t){ ACTION_CONSUMER_TAP, USB_MOD_NONE, USB_KEY_NONE, USB_CONSUMER_NEXT_TRACK, 0, 0, 0, 0 };
        l0->bindings[l0->binding_count++] = b;
    }

    // Preset layers 1..4 (inherit Layer 0 by default). All are persistent so a
    // selected configuration stays active until the user switches again.
    strncpy(engine->layers[1].name, "配置1", sizeof(engine->layers[1].name) - 1);
    engine->layers[1].type = LAYER_TYPE_PERSISTENT;
    engine->layers[1].timeout_sec = 0;
    engine->layers[1].led_color = 0x06B6D4;
    engine->layers[1].binding_count = 0;

    strncpy(engine->layers[2].name, "配置2", sizeof(engine->layers[2].name) - 1);
    engine->layers[2].type = LAYER_TYPE_PERSISTENT;
    engine->layers[2].timeout_sec = 0;
    engine->layers[2].led_color = 0xA855F7;
    engine->layers[2].binding_count = 0;

    strncpy(engine->layers[3].name, "配置3", sizeof(engine->layers[3].name) - 1);
    engine->layers[3].type = LAYER_TYPE_PERSISTENT;
    engine->layers[3].timeout_sec = 0;
    engine->layers[3].led_color = 0xEAB308;
    engine->layers[3].binding_count = 0;

    strncpy(engine->layers[4].name, "配置4", sizeof(engine->layers[4].name) - 1);
    engine->layers[4].type = LAYER_TYPE_PERSISTENT;
    engine->layers[4].timeout_sec = 0;
    engine->layers[4].led_color = 0xFFFFFF;
    engine->layers[4].binding_count = 0;
}

bool key_engine_set_layer_binding(key_mapper_engine_t *engine, uint8_t layer_idx, const key_binding_t *binding)
{
    if (!engine || !binding || layer_idx >= MAX_LAYERS) return false;
    key_engine_lock();

    key_layer_t *layer = &engine->layers[layer_idx];
    int idx = find_binding_index_in_layer(layer, binding->source_vk);
    if (idx >= 0) {
        layer->bindings[idx] = *binding;
        key_engine_unlock();
        return true;
    }
    if (layer->binding_count < MAX_KEY_BINDINGS) {
        layer->bindings[layer->binding_count++] = *binding;
        key_engine_unlock();
        return true;
    }
    key_engine_unlock();
    return false;
}

bool key_engine_get_layer_binding(const key_mapper_engine_t *engine, uint8_t layer_idx, uint8_t source_vk, key_binding_t *out_binding)
{
    if (!engine || !out_binding || layer_idx >= MAX_LAYERS) return false;
    key_engine_lock();

    const key_layer_t *layer = &engine->layers[layer_idx];
    int idx = find_binding_index_in_layer(layer, source_vk);
    if (idx >= 0) {
        *out_binding = layer->bindings[idx];
        key_engine_unlock();
        return true;
    }
    key_engine_unlock();
    return false;
}

bool key_engine_set_binding(key_mapper_engine_t *engine, const key_binding_t *binding)
{
    return key_engine_set_layer_binding(engine, 0, binding);
}

bool key_engine_get_binding(const key_mapper_engine_t *engine, uint8_t source_vk, key_binding_t *out_binding)
{
    return key_engine_get_layer_binding(engine, 0, source_vk, out_binding);
}

void key_engine_init(key_mapper_engine_t *engine, key_output_callback_t cb)
{
    if (!engine) return;
    key_engine_lock();
    memset(engine, 0, sizeof(key_mapper_engine_t));
    engine->output_cb = cb;
    key_engine_load_defaults(engine);
    key_engine_unlock();
}

void key_engine_feed_key(key_mapper_engine_t *engine, uint8_t raw_key_code, bool is_pressed, uint32_t now_ms)
{
    if (!engine) return;
    key_engine_lock();

    s_now_ms = now_ms;
    engine->last_activity_time = now_ms;

    int slot = get_physical_key_slot(raw_key_code);
    if (slot < 0 || slot >= MAX_KEY_BINDINGS) {
        key_engine_unlock();
        return;
    }

    uint8_t can_vk = canonical_source_vk(raw_key_code);

    // Modal configuration-switch mode: only keys in the global switch map and
    // the BACK key act, and no normal HID action is emitted.
    if (engine->switch_mode_active) {
        key_slot_state_t *s = &engine->states[slot];
        if (is_pressed) {
            if (!s->is_pressed) {
                s->is_pressed = true;
                s->press_timestamp = now_ms;
                // Any activity restarts the inactivity timeout.
                engine->switch_mode_last_activity_ms = now_ms;

                // A key exits the mode only when the CURRENT configuration
                // binds it to the switch action; a key merely configured in
                // another configuration does not. The TV key is the universal
                // fallback (rapid-press entry), so it always exits once the
                // lockout has elapsed. BACK always cancels.
                key_binding_t sb;
                get_effective_binding(engine, raw_key_code, &sb);
                bool is_trigger =
                    (sb.has_click && sb.click_action.type == ACTION_ENTER_SWITCH_MODE) ||
                    (sb.has_long && sb.long_action.type == ACTION_ENTER_SWITCH_MODE) ||
                    (sb.has_double && sb.double_action.type == ACTION_ENTER_SWITCH_MODE);
                // Entering via the rapid-TV shortcut must not immediately exit
                // on the tail of that same rapid sequence.
                bool tv_locked = (can_vk == MI_KEY_TV) &&
                    (now_ms - engine->switch_mode_enter_ms) < SWITCH_MODE_TV_LOCKOUT_MS;
                // TV only exits when it was the rapid-press entry key, or when
                // the current configuration binds it to the switch action.
                bool tv_exit = (can_vk == MI_KEY_TV) &&
                    (engine->switch_mode_via_tv_rapid || is_trigger);
                bool exit_now = (can_vk == MI_KEY_BACK) ||
                    (!tv_locked && (is_trigger || tv_exit));

                if (exit_now) {
                    key_engine_exit_switch_mode(engine);
                } else {
                    // The confirm key is locked to the default configuration.
                    int target = (can_vk == MI_KEY_OK) ? 0 : find_switch_target(engine, can_vk);
                    if (target >= 0) {
                        engine->switch_mode_active = false;
                        led_indicator_set_switch_mode(false);
                        engine->states[slot].is_pressed = false;
                        key_engine_switch_layer(engine, (uint8_t)target, now_ms);
                    }
                }
            }
        } else {
            s->is_pressed = false;
        }
        key_engine_unlock();
        return;
    }

    // Fallback: five rapid TV presses force the switch mode so a configuration
    // that never bound the switch action can still be escaped.
    if (is_pressed && can_vk == MI_KEY_TV) {
        if (s_tv_rapid_count == 0 || (now_ms - s_tv_rapid_first_ms) > TV_RAPID_WINDOW_MS) {
            s_tv_rapid_count = 1;
            s_tv_rapid_first_ms = now_ms;
        } else {
            s_tv_rapid_count++;
        }
        if (s_tv_rapid_count >= TV_RAPID_PRESS_COUNT) {
            s_tv_rapid_count = 0;
            key_engine_enter_switch_mode(engine, now_ms, true);
            key_engine_unlock();
            return;
        }
    }

    key_binding_t b;
    get_effective_binding(engine, raw_key_code, &b);
    key_slot_state_t *s = &engine->states[slot];

    engine->last_telemetry.source_vk = raw_key_code;
    engine->last_telemetry.is_pressed = is_pressed;
    engine->last_telemetry.timestamp = now_ms;
    engine->last_telemetry.action_type = b.click_action.type;
    engine->last_telemetry.modifier = b.click_action.modifier;
    engine->last_telemetry.key_code = b.click_action.key_code;
    engine->last_telemetry.consumer_code = b.click_action.consumer_code;
    engine->last_telemetry.active_layer = engine->active_layer;

    if (is_pressed) {
        if (!s->is_pressed) {
            s->is_pressed = true;
            s->press_timestamp = now_ms;
            s->long_fired = false;

            if (engine->active_layer != 0) {
                app_log("KEYMAP", "Key 0x%02X down: layer=%u click_type=%u",
                        raw_key_code, engine->active_layer, (unsigned)b.click_action.type);
            }

            if (b.has_repeat) {
                s->next_repeat_timestamp = now_ms + b.repeat_delay_ms;
            }

            if (!b.has_long && !b.has_double) {
                if (b.click_action.type == ACTION_MOUSE_MOVE) {
                    // Move one step now, then keep moving while held.
                    emit_action(engine, &b.click_action, raw_key_code, true);
                    s->move_active = true;
                    s->next_move_timestamp = now_ms + MOUSE_MOVE_INTERVAL_MS;
                } else if (b.click_action.type == ACTION_KEYBOARD_HOLD ||
                    b.click_action.type == ACTION_CONSUMER_HOLD ||
                    b.click_action.type == ACTION_VOICE_HOLD ||
                    b.click_action.type == ACTION_SWITCH_LAYER ||
                    b.click_action.type == ACTION_MOUSE_BUTTON_HOLD ||
                    b.click_action.type == ACTION_MOUSE_WHEEL) {
                    emit_action(engine, &b.click_action, raw_key_code, true);
                } else if (b.has_click &&
                           (b.click_action.type == ACTION_KEYBOARD_TAP ||
                            b.click_action.type == ACTION_CONSUMER_TAP ||
                            b.click_action.type == ACTION_MOUSE_BUTTON_TAP ||
                            b.click_action.type == ACTION_ENTER_SWITCH_MODE)) {
                    // Click mappings fire immediately on press so the host
                    // reacts without waiting for the key-up. The release branch
                    // below deliberately does not re-emit them, so holding the
                    // physical key cannot trigger the click twice.
                    emit_action(engine, &b.click_action, raw_key_code, true);
                }
            }
        }
    } else {
        if (s->is_pressed) {
            s->is_pressed = false;
            s->move_active = false;
            s->release_timestamp = now_ms;
            uint32_t duration = now_ms - s->press_timestamp;
            engine->last_telemetry.duration_ms = duration;

            if (!b.has_long && !b.has_double) {
                if (b.click_action.type == ACTION_KEYBOARD_HOLD) {
                    key_action_t rel = { ACTION_KEYBOARD_RELEASE, 0, 0, 0, 0, 0, 0, 0 };
                    emit_action(engine, &rel, raw_key_code, false);
                } else if (b.click_action.type == ACTION_CONSUMER_HOLD) {
                    key_action_t rel = { ACTION_CONSUMER_RELEASE, 0, 0, 0, 0, 0, 0, 0 };
                    emit_action(engine, &rel, raw_key_code, false);
                } else if (b.click_action.type == ACTION_VOICE_HOLD) {
                    key_action_t rel = { ACTION_VOICE_RELEASE, 0, 0, 0, 0, 0, 0, 0 };
                    emit_action(engine, &rel, raw_key_code, false);
                } else if (b.click_action.type == ACTION_MOUSE_BUTTON_HOLD) {
                    key_action_t rel = { ACTION_MOUSE_BUTTON_RELEASE, 0, b.click_action.key_code, 0, 0, 0, 0, 0 };
                    emit_action(engine, &rel, raw_key_code, false);
                } else if (b.has_click &&
                           (b.click_action.type == ACTION_KEYBOARD_RELEASE ||
                            b.click_action.type == ACTION_CONSUMER_RELEASE)) {
                    // Explicit release actions still fire on key-up.
                    emit_action(engine, &b.click_action, raw_key_code, false);
                }
                // Keyboard/consumer/mouse TAP, mouse move, wheel and layer
                // switch are emitted on press; nothing to do on release.
            } else {
                if (s->long_fired) {
                    if (b.long_action.type == ACTION_KEYBOARD_HOLD) {
                        key_action_t rel = { ACTION_KEYBOARD_RELEASE, 0, 0, 0, 0, 0, 0, 0 };
                        emit_action(engine, &rel, raw_key_code, false);
                    } else if (b.long_action.type == ACTION_CONSUMER_HOLD) {
                        key_action_t rel = { ACTION_CONSUMER_RELEASE, 0, 0, 0, 0, 0, 0, 0 };
                        emit_action(engine, &rel, raw_key_code, false);
                    } else if (b.long_action.type == ACTION_MOUSE_BUTTON_HOLD) {
                        key_action_t rel = { ACTION_MOUSE_BUTTON_RELEASE, 0, b.long_action.key_code, 0, 0, 0, 0, 0 };
                        emit_action(engine, &rel, raw_key_code, false);
                    }
                } else if (b.has_click || b.has_double) {
                    if (!b.has_double) {
                        emit_action_as_tap_if_hold(engine, &b.click_action, raw_key_code);
                    } else {
                        s->press_count++;
                        if (s->press_count == 1) {
                            s->waiting_double = true;
                        } else if (s->press_count >= 2) {
                            s->waiting_double = false;
                            s->press_count = 0;
                            emit_action_as_tap_if_hold(engine, &b.double_action, raw_key_code);
                        }
                    }
                }
            }
        }
    }
    key_engine_unlock();
}

// Physical slot -> canonical key code, indexed by slot. This is the single
// source of truth for the slot numbering: get_physical_key_slot() assigns these
// slot numbers, and the tick, release-all and telemetry paths all read the slot
// back through this table. Keeping one copy means adding a button cannot leave
// one of the three paths behind (previously each had its own array and a
// hard-coded count of 13, which silently excluded any higher slot).
static const uint8_t k_slot_keys[] = {
    MI_KEY_POWER,           // 0
    MI_KEY_VOICE,           // 1
    MI_KEY_UP,              // 2
    MI_KEY_DOWN,            // 3
    MI_KEY_LEFT,            // 4
    MI_KEY_RIGHT,           // 5
    MI_KEY_OK,              // 6
    MI_KEY_BACK,            // 7
    MI_KEY_HOME,            // 8
    MI_KEY_MENU,            // 9
    MI_KEY_VOL_UP,          // 10
    MI_KEY_VOL_DOWN,        // 11
    MI_KEY_TV,              // 12  (input / source select)
    MI_KEY_GTV_MUTE,        // 13
    MI_KEY_GTV_APP_1,       // 14
    MI_KEY_GTV_APP_2,       // 15
};

#define KEY_SLOT_COUNT ((int)(sizeof(k_slot_keys) / sizeof(k_slot_keys[0])))

void key_engine_tick(key_mapper_engine_t *engine, uint32_t now_ms)
{
    if (!engine) return;
    key_engine_lock();

    s_now_ms = now_ms;

    // Auto-exit the configuration-switch mode after a period of inactivity.
    if (engine->switch_mode_active &&
        (now_ms - engine->switch_mode_last_activity_ms) >= SWITCH_MODE_TIMEOUT_MS) {
        engine->switch_mode_active = false;
        memset(engine->states, 0, sizeof(engine->states));
        led_indicator_set_switch_mode(false);
        app_log("KEYMAP", "Config switch mode timed out");
    }

    if (engine->active_layer != 0 && engine->layers[engine->active_layer].type == LAYER_TYPE_TIMEOUT) {
        uint32_t timeout_ms = (uint32_t)engine->layers[engine->active_layer].timeout_sec * 1000;
        if (timeout_ms > 0 && (now_ms - engine->last_activity_time >= timeout_ms)) {
            app_log("KEYMAP", "Layer %u timed out (%u s) -> reverting to Layer 0",
                    engine->active_layer, engine->layers[engine->active_layer].timeout_sec);
            key_engine_switch_layer(engine, 0, now_ms);
        }
    }

    for (int slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        key_slot_state_t *s = &engine->states[slot];
        if (!s->is_pressed && !s->waiting_double) continue;

        uint8_t raw_key = k_slot_keys[slot];
        key_binding_t b;
        get_effective_binding(engine, raw_key, &b);

        if (s->is_pressed) {
            uint32_t hold_time = now_ms - s->press_timestamp;
            if (b.has_long && !s->long_fired && hold_time >= b.long_ms) {
                s->long_fired = true;
                emit_action(engine, &b.long_action, b.source_vk, true);
            }
            if (s->move_active && b.click_action.type == ACTION_MOUSE_MOVE) {
                if (now_ms >= s->next_move_timestamp) {
                    emit_action(engine, &b.click_action, b.source_vk, true);
                    s->next_move_timestamp = now_ms + MOUSE_MOVE_INTERVAL_MS;
                }
            } else if (b.has_repeat && now_ms >= s->next_repeat_timestamp) {
                emit_action(engine, &b.repeat_action, b.source_vk, true);
                s->next_repeat_timestamp = now_ms + b.repeat_interval_ms;
            }
        } else {
            if (s->waiting_double && (now_ms - s->release_timestamp >= b.double_ms)) {
                s->waiting_double = false;
                s->press_count = 0;
                emit_action_as_tap_if_hold(engine, &b.click_action, b.source_vk);
            }
        }
    }
    key_engine_unlock();
}

uint8_t key_engine_get_pressed_vk(const key_mapper_engine_t *engine)
{
    uint8_t vk = 0;
    if (!engine) return 0;
    key_engine_lock();
    for (int slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        if (engine->states[slot].is_pressed) {
            vk = k_slot_keys[slot];
            break;
        }
    }
    key_engine_unlock();
    return vk;
}

int key_engine_slot_count(void)
{
    return KEY_SLOT_COUNT;
}

uint8_t key_engine_vk_for_slot(int slot)
{
    if (slot < 0 || slot >= KEY_SLOT_COUNT) return 0;
    return k_slot_keys[slot];
}

int key_engine_slot_for_vk(uint8_t vk)
{
    for (int slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        if (k_slot_keys[slot] == vk) return slot;
    }

    // A Google TV Remote reports its own codes, and those carry the slot
    // directly (slot = code - MI_KEY_GTV_POWER). They have to resolve here too,
    // because a caller holding a raw report never sees the canonical code the
    // table above stores. Mute / app1 / app2 are in both tables and were
    // already matched by the loop, so only the first thirteen reach this path.
    if (vk >= MI_KEY_GTV_POWER && vk <= MI_KEY_GTV_APP_2) {
        return (int)(vk - MI_KEY_GTV_POWER);
    }
    return -1;
}

void key_engine_release_all(key_mapper_engine_t *engine, uint32_t now_ms)
{
    if (!engine) return;
    key_engine_lock();

    for (int slot = 0; slot < KEY_SLOT_COUNT; slot++) {
        key_slot_state_t *s = &engine->states[slot];
        if (s->is_pressed) {
            key_engine_feed_key(engine, k_slot_keys[slot], false, now_ms);
        }
        s->waiting_double = false;
        s->press_count = 0;
        s->move_active = false;
    }

    if (engine->switch_mode_active) {
        engine->switch_mode_active = false;
        led_indicator_set_switch_mode(false);
    }
    key_engine_unlock();
}
