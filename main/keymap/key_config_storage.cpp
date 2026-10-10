#include "key_config_storage.h"
#include "app_log.h"
#include "led/led_indicator.h"
#include "storage/config_store.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define KEYMAP_NS "keymap_conf"

extern key_mapper_engine_t g_key_engine;

// Compact per-layer blob: header + only the used bindings (instead of a fixed
// 16-slot array), which cuts the NVS write from ~5.6 KB to ~1 KB.
#define LAYER_BLOB_MAGIC 0x4D4C5941u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  binding_count;
    uint8_t  type;
    uint16_t timeout_sec;
    uint32_t led_color;
    char     name[MAX_LAYER_NAME_LEN];
} layer_hdr_t;

#define LAYER_BLOB_MAX (sizeof(layer_hdr_t) + MAX_KEY_BINDINGS * sizeof(key_binding_t))

// Global configuration-switch map blob (see ACTION_ENTER_SWITCH_MODE).
#define SWITCH_BLOB_MAGIC 0x53574D41u
#define SWITCH_NVS_KEY    "switch"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  count;
    uint8_t  reserved[3];
    key_switch_map_entry_t entries[MAX_SWITCH_MAP];
} switch_blob_t;

// Global voice settings blob. Kept apart from the layer blobs so adding it did
// not change their on-flash layout; an absent blob means factory defaults.
#define VOICE_BLOB_MAGIC 0x56434647u
#define VOICE_NVS_KEY    "voice"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  trigger;
    uint8_t  hotkey_mode;
    uint16_t max_sec;
} voice_blob_t;

static SemaphoreHandle_t s_save_sem = NULL;

// Shadow of what is currently in NVS, per key. A save skips any key whose bytes
// are unchanged: flash wear is dominated by how often a page gets programmed,
// not by how much is written, so rewriting an untouched layer is pure cost.
// Without this, editing one layer rewrote all seven keys.
static uint8_t *s_shadow[MAX_LAYERS];
static size_t   s_shadow_len[MAX_LAYERS];
static uint8_t *s_switch_shadow = NULL;
static size_t   s_switch_shadow_len = 0;
static voice_blob_t s_voice_shadow;
static bool     s_voice_shadow_valid = false;

// Forget the shadow so the next save rewrites everything (a blob is never zero
// bytes long, so a zero length reliably means "unknown").
static void shadow_invalidate(void)
{
    for (int i = 0; i < MAX_LAYERS; i++) {
        s_shadow_len[i] = 0;
    }
    s_switch_shadow_len = 0;
    s_voice_shadow_valid = false;
}

static void voice_config_sanitize(key_voice_config_t *v)
{
    if (v->trigger > VOICE_TRIGGER_HOLD) v->trigger = VOICE_TRIGGER_TOGGLE;
    if (v->hotkey_mode > VOICE_HOTKEY_TAP_START) v->hotkey_mode = VOICE_HOTKEY_HOLD;
    if (v->max_sec > VOICE_MAX_SEC_LIMIT) v->max_sec = VOICE_MAX_SEC_LIMIT;
}

static void shadow_alloc(void)
{
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!s_shadow[i]) {
            s_shadow[i] = (uint8_t *)heap_caps_malloc(LAYER_BLOB_MAX, MALLOC_CAP_SPIRAM);
        }
    }
    if (!s_switch_shadow) {
        s_switch_shadow = (uint8_t *)heap_caps_malloc(sizeof(switch_blob_t), MALLOC_CAP_SPIRAM);
    }
}

static void layer_key(char *buf, size_t buf_len, int idx)
{
    snprintf(buf, buf_len, "layer%d", idx);
}

static uint32_t parse_u32_or_hex(JsonVariant v, uint32_t default_val = 0)
{
    if (v.isNull()) return default_val;
    if (v.is<int>() || v.is<unsigned int>() || v.is<long>() || v.is<unsigned long>()) {
        return v.as<uint32_t>();
    }
    if (v.is<const char *>() || v.is<std::string>()) {
        std::string s = v.as<std::string>();
        if (!s.empty() && (s[0] == '0') && (s.size() > 1) && (s[1] == 'x' || s[1] == 'X')) {
            return (uint32_t)strtoul(s.c_str(), NULL, 16);
        }
        return (uint32_t)strtoul(s.c_str(), NULL, 10);
    }
    return default_val;
}

// Signed variant: ArduinoJson's as<uint32_t>() returns 0 for negative values,
// which would silently drop mouse deltas (left / up / scroll-up).
static int32_t parse_i32(JsonVariant v, int32_t default_val = 0)
{
    if (v.isNull()) return default_val;
    if (v.is<int>() || v.is<long>()) {
        return (int32_t)v.as<long>();
    }
    if (v.is<unsigned int>() || v.is<unsigned long>()) {
        return (int32_t)v.as<unsigned long>();
    }
    if (v.is<const char *>() || v.is<std::string>()) {
        std::string s = v.as<std::string>();
        return (int32_t)strtol(s.c_str(), NULL, 0);
    }
    return default_val;
}

static void parse_bindings_array(JsonArray arr, key_layer_t *layer)
{
    if (!layer || arr.isNull()) return;
    layer->binding_count = 0;
    for (JsonObject obj : arr) {
        if (layer->binding_count >= MAX_KEY_BINDINGS) break;

        key_binding_t b;
        memset(&b, 0, sizeof(b));

        b.source_vk = parse_u32_or_hex(obj["source_vk"], 0);

        b.has_click = obj["has_click"] | false;
        b.click_action.type = (key_action_type_t)parse_u32_or_hex(obj["click_type"], 0);
        b.click_action.modifier = (uint8_t)parse_u32_or_hex(obj["click_mod"], 0);
        b.click_action.key_code = (uint8_t)parse_u32_or_hex(obj["click_key"], 0);
        b.click_action.consumer_code = (uint16_t)parse_u32_or_hex(obj["click_cons"], 0);
        b.click_action.target_layer = (uint8_t)parse_u32_or_hex(obj["click_layer"], 0);
        b.click_action.mouse_dx = (int8_t)parse_i32(obj["click_dx"], 0);
        b.click_action.mouse_dy = (int8_t)parse_i32(obj["click_dy"], 0);
        b.click_action.mouse_wheel = (int8_t)parse_i32(obj["click_wheel"], 0);

        // Normalize HID voice usage (0x3E) to the canonical voice code (0x04).
        if (b.source_vk == MI_KEY_VOICE_ALT) {
            b.source_vk = MI_KEY_VOICE;
        }

        // Force ACTION_VOICE_HOLD for the voice key unless it switches layers.
        if (b.source_vk == MI_KEY_VOICE && b.click_action.type != ACTION_SWITCH_LAYER &&
            b.click_action.type != ACTION_TRANSPARENT) {
            b.click_action.type = ACTION_VOICE_HOLD;
        }

        b.has_long = obj["has_long"] | false;
        b.long_ms = parse_u32_or_hex(obj["long_ms"], 600);
        b.long_action.type = (key_action_type_t)parse_u32_or_hex(obj["long_type"], 1);
        b.long_action.modifier = (uint8_t)parse_u32_or_hex(obj["long_mod"], 0);
        b.long_action.key_code = (uint8_t)parse_u32_or_hex(obj["long_key"], 0);
        b.long_action.consumer_code = (uint16_t)parse_u32_or_hex(obj["long_cons"], 0);
        b.long_action.target_layer = (uint8_t)parse_u32_or_hex(obj["long_layer"], 0);
        b.long_action.mouse_dx = (int8_t)parse_i32(obj["long_dx"], 0);
        b.long_action.mouse_dy = (int8_t)parse_i32(obj["long_dy"], 0);
        b.long_action.mouse_wheel = (int8_t)parse_i32(obj["long_wheel"], 0);

        b.has_double = obj["has_double"] | false;
        b.double_ms = parse_u32_or_hex(obj["double_ms"], 250);
        b.double_action.type = (key_action_type_t)parse_u32_or_hex(obj["double_type"], 1);
        b.double_action.modifier = (uint8_t)parse_u32_or_hex(obj["double_mod"], 0);
        b.double_action.key_code = (uint8_t)parse_u32_or_hex(obj["double_key"], 0);
        b.double_action.consumer_code = (uint16_t)parse_u32_or_hex(obj["double_cons"], 0);
        b.double_action.target_layer = (uint8_t)parse_u32_or_hex(obj["double_layer"], 0);
        b.double_action.mouse_dx = (int8_t)parse_i32(obj["double_dx"], 0);
        b.double_action.mouse_dy = (int8_t)parse_i32(obj["double_dy"], 0);
        b.double_action.mouse_wheel = (int8_t)parse_i32(obj["double_wheel"], 0);

        b.has_repeat = obj["has_repeat"] | false;
        b.repeat_action.type = (key_action_type_t)parse_u32_or_hex(obj["repeat_type"], 0);
        b.repeat_action.modifier = (uint8_t)parse_u32_or_hex(obj["repeat_mod"], 0);
        b.repeat_action.key_code = (uint8_t)parse_u32_or_hex(obj["repeat_key"], 0);
        b.repeat_action.consumer_code = (uint16_t)parse_u32_or_hex(obj["repeat_cons"], 0);
        b.repeat_action.mouse_dx = (int8_t)parse_i32(obj["repeat_dx"], 0);
        b.repeat_action.mouse_dy = (int8_t)parse_i32(obj["repeat_dy"], 0);
        b.repeat_action.mouse_wheel = (int8_t)parse_i32(obj["repeat_wheel"], 0);
        b.repeat_delay_ms = (uint16_t)parse_u32_or_hex(obj["repeat_delay_ms"], 350);
        b.repeat_interval_ms = (uint16_t)parse_u32_or_hex(obj["repeat_interval_ms"], 70);

        layer->bindings[layer->binding_count++] = b;
    }
}

size_t key_config_to_json(const key_mapper_engine_t *engine, char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!engine) return 0;

    JsonDocument doc;
    doc["active_layer"] = engine->active_layer;
    JsonArray layer_arr = doc["layers"].to<JsonArray>();

    for (size_t l = 0; l < MAX_LAYERS; l++) {
        const key_layer_t *layer = &engine->layers[l];
        JsonObject layer_obj = layer_arr.add<JsonObject>();
        layer_obj["id"] = (int)l;
        layer_obj["name"] = layer->name;
        layer_obj["type"] = (int)layer->type;
        layer_obj["timeout"] = layer->timeout_sec;

        char color_buf[16];
        snprintf(color_buf, sizeof(color_buf), "0x%06X", (unsigned int)(layer->led_color & 0xFFFFFF));
        layer_obj["color"] = color_buf;

        JsonArray arr = layer_obj["bindings"].to<JsonArray>();
        for (size_t i = 0; i < layer->binding_count; i++) {
            const key_binding_t *b = &layer->bindings[i];
            JsonObject obj = arr.add<JsonObject>();
            obj["source_vk"] = b->source_vk;

            if (b->has_click) {
                obj["has_click"] = true;
                obj["click_type"] = (int)b->click_action.type;
                if (b->click_action.modifier != 0) obj["click_mod"] = b->click_action.modifier;
                if (b->click_action.key_code != 0) obj["click_key"] = b->click_action.key_code;
                if (b->click_action.consumer_code != 0) obj["click_cons"] = b->click_action.consumer_code;
                if (b->click_action.type == ACTION_SWITCH_LAYER) obj["click_layer"] = b->click_action.target_layer;
                if (b->click_action.mouse_dx != 0) obj["click_dx"] = b->click_action.mouse_dx;
                if (b->click_action.mouse_dy != 0) obj["click_dy"] = b->click_action.mouse_dy;
                if (b->click_action.mouse_wheel != 0) obj["click_wheel"] = b->click_action.mouse_wheel;
            }
            if (b->has_long) {
                obj["has_long"] = true;
                obj["long_ms"] = b->long_ms;
                obj["long_type"] = (int)b->long_action.type;
                if (b->long_action.modifier != 0) obj["long_mod"] = b->long_action.modifier;
                if (b->long_action.key_code != 0) obj["long_key"] = b->long_action.key_code;
                if (b->long_action.consumer_code != 0) obj["long_cons"] = b->long_action.consumer_code;
                if (b->long_action.type == ACTION_SWITCH_LAYER) obj["long_layer"] = b->long_action.target_layer;
                if (b->long_action.mouse_dx != 0) obj["long_dx"] = b->long_action.mouse_dx;
                if (b->long_action.mouse_dy != 0) obj["long_dy"] = b->long_action.mouse_dy;
                if (b->long_action.mouse_wheel != 0) obj["long_wheel"] = b->long_action.mouse_wheel;
            }
            if (b->has_double) {
                obj["has_double"] = true;
                obj["double_ms"] = b->double_ms;
                obj["double_type"] = (int)b->double_action.type;
                if (b->double_action.modifier != 0) obj["double_mod"] = b->double_action.modifier;
                if (b->double_action.key_code != 0) obj["double_key"] = b->double_action.key_code;
                if (b->double_action.consumer_code != 0) obj["double_cons"] = b->double_action.consumer_code;
                if (b->double_action.type == ACTION_SWITCH_LAYER) obj["double_layer"] = b->double_action.target_layer;
                if (b->double_action.mouse_dx != 0) obj["double_dx"] = b->double_action.mouse_dx;
                if (b->double_action.mouse_dy != 0) obj["double_dy"] = b->double_action.mouse_dy;
                if (b->double_action.mouse_wheel != 0) obj["double_wheel"] = b->double_action.mouse_wheel;
            }
            if (b->has_repeat) {
                obj["has_repeat"] = true;
                obj["repeat_type"] = (int)b->repeat_action.type;
                if (b->repeat_action.modifier != 0) obj["repeat_mod"] = b->repeat_action.modifier;
                if (b->repeat_action.key_code != 0) obj["repeat_key"] = b->repeat_action.key_code;
                if (b->repeat_action.consumer_code != 0) obj["repeat_cons"] = b->repeat_action.consumer_code;
                if (b->repeat_action.mouse_dx != 0) obj["repeat_dx"] = b->repeat_action.mouse_dx;
                if (b->repeat_action.mouse_dy != 0) obj["repeat_dy"] = b->repeat_action.mouse_dy;
                if (b->repeat_action.mouse_wheel != 0) obj["repeat_wheel"] = b->repeat_action.mouse_wheel;
                obj["repeat_delay_ms"] = b->repeat_delay_ms;
                obj["repeat_interval_ms"] = b->repeat_interval_ms;
            }
        }
    }

    JsonArray sm = doc["switch_map"].to<JsonArray>();
    for (size_t i = 0; i < engine->switch_map_count && i < MAX_SWITCH_MAP; i++) {
        JsonObject e = sm.add<JsonObject>();
        e["source_vk"] = engine->switch_map[i].source_vk;
        e["layer"] = engine->switch_map[i].target_layer;
    }

    JsonObject voice = doc["voice"].to<JsonObject>();
    voice["trigger"] = engine->voice.trigger;
    voice["hotkey_mode"] = engine->voice.hotkey_mode;
    voice["max_sec"] = engine->voice.max_sec;

    return serializeJson(doc, out, out_len);
}

bool key_config_from_json(key_mapper_engine_t *engine, const char *json_str)
{
    if (!engine || !json_str || json_str[0] == '\0') return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_str);
    if (err) {
        app_log("KEYMAP", "JSON deserialize failed: %s", err.c_str());
        return false;
    }

    // Parse into a scratch copy so the live engine is never half-updated.
    // Allocated from PSRAM: sizeof(key_layer_t) * MAX_LAYERS is ~6 KB, which
    // would overflow the WebUSB task stack if kept as a local array.
    key_layer_t *tmp = (key_layer_t *)heap_caps_malloc(sizeof(key_layer_t) * MAX_LAYERS,
                                                       MALLOC_CAP_SPIRAM);
    if (!tmp) {
        app_log("KEYMAP", "no memory for scratch keymap");
        return false;
    }
    key_engine_lock();
    memcpy(tmp, engine->layers, sizeof(key_layer_t) * MAX_LAYERS);
    key_engine_unlock();

    key_switch_map_entry_t smap[MAX_SWITCH_MAP];
    size_t smap_n = 0;
    bool smap_present = false;

    bool applied = false;
    if (doc["layers"].is<JsonArray>()) {
        for (JsonObject l_obj : doc["layers"].as<JsonArray>()) {
            uint8_t id = (uint8_t)parse_u32_or_hex(l_obj["id"], 0);
            if (id >= MAX_LAYERS) continue;

            key_layer_t *layer = &tmp[id];
            if (!l_obj["name"].isNull()) {
                const char *nm = l_obj["name"].as<const char *>();
                if (nm) {
                    strncpy(layer->name, nm, sizeof(layer->name) - 1);
                    layer->name[sizeof(layer->name) - 1] = '\0';
                }
            }
            layer->type = (layer_type_t)parse_u32_or_hex(l_obj["type"], (uint32_t)layer->type);
            layer->timeout_sec = (uint16_t)parse_u32_or_hex(l_obj["timeout"], layer->timeout_sec);
            if (!l_obj["color"].isNull()) {
                layer->led_color = parse_u32_or_hex(l_obj["color"], layer->led_color);
            }
            JsonArray b_arr = l_obj["bindings"].as<JsonArray>();
            if (!b_arr.isNull()) {
                parse_bindings_array(b_arr, layer);
            }
        }
        applied = true;
    } else if (doc["bindings"].is<JsonArray>()) {
        parse_bindings_array(doc["bindings"].as<JsonArray>(), &tmp[0]);
        applied = true;
    }

    if (doc["switch_map"].is<JsonArray>()) {
        smap_present = true;
        for (JsonObject e : doc["switch_map"].as<JsonArray>()) {
            if (smap_n >= MAX_SWITCH_MAP) break;
            uint8_t vk = (uint8_t)parse_u32_or_hex(e["source_vk"], 0);
            uint32_t layer = parse_u32_or_hex(e["layer"], 0);
            if (vk == 0 || layer >= MAX_LAYERS) continue;
            if (vk == MI_KEY_VOICE_ALT) vk = MI_KEY_VOICE;
            smap[smap_n].source_vk = vk;
            smap[smap_n].target_layer = (uint8_t)layer;
            smap_n++;
        }
    }

    // Voice settings may be sent on their own, without layers.
    key_engine_lock();
    key_voice_config_t voice = engine->voice;
    key_engine_unlock();
    bool voice_present = doc["voice"].is<JsonObject>();
    if (voice_present) {
        JsonObject v = doc["voice"].as<JsonObject>();
        voice.trigger = (uint8_t)parse_u32_or_hex(v["trigger"], voice.trigger);
        voice.hotkey_mode = (uint8_t)parse_u32_or_hex(v["hotkey_mode"], voice.hotkey_mode);
        voice.max_sec = (uint16_t)parse_u32_or_hex(v["max_sec"], voice.max_sec);
        voice_config_sanitize(&voice);
        if (!applied) {
            key_engine_lock();
            engine->voice = voice;
            engine->config_rev++;
            key_engine_unlock();
            heap_caps_free(tmp);
            app_log("KEYMAP", "Voice settings applied: trigger=%u hotkey=%u max=%us",
                    voice.trigger, voice.hotkey_mode, voice.max_sec);
            return true;
        }
    }

    if (applied) {
        app_log("KEYMAP", "Parsed L0=%u L1=%u L2=%u L3=%u L4=%u bindings",
                (unsigned)tmp[0].binding_count, (unsigned)tmp[1].binding_count,
                (unsigned)tmp[2].binding_count, (unsigned)tmp[3].binding_count,
                (unsigned)tmp[4].binding_count);
    }

    if (!applied) {
        heap_caps_free(tmp);
        return false;
    }

    key_engine_lock();
    memcpy(engine->layers, tmp, sizeof(key_layer_t) * MAX_LAYERS);
    engine->layer_count = MAX_LAYERS;
    memset(engine->states, 0, sizeof(engine->states));
    if (smap_present) {
        memcpy(engine->switch_map, smap, smap_n * sizeof(key_switch_map_entry_t));
        engine->switch_map_count = smap_n;
    }
    if (voice_present) {
        engine->voice = voice;
    }
    engine->config_rev++;
    uint32_t color = engine->layers[engine->active_layer].led_color;
    key_engine_unlock();

    heap_caps_free(tmp);
    led_indicator_set_layer_color(color);
    app_log("KEYMAP", "Keymap applied from JSON");
    return true;
}

bool key_config_storage_save(key_mapper_engine_t *engine)
{
    if (!engine) return false;

    shadow_alloc();
    uint8_t *buf = (uint8_t *)heap_caps_malloc(LAYER_BLOB_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        app_log("KEYMAP", "no memory for save");
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(KEYMAP_NS, NVS_READWRITE, &h) != ESP_OK) {
        heap_caps_free(buf);
        app_log("KEYMAP", "nvs_open failed");
        return false;
    }

    key_engine_lock();
    bool ok = true;
    int written = 0;
    for (int i = 0; i < MAX_LAYERS; i++) {
        key_layer_t *layer = &engine->layers[i];
        layer_hdr_t *hdr = (layer_hdr_t *)buf;
        hdr->magic = LAYER_BLOB_MAGIC;
        hdr->binding_count = (uint8_t)layer->binding_count;
        hdr->type = (uint8_t)layer->type;
        hdr->timeout_sec = layer->timeout_sec;
        hdr->led_color = layer->led_color;
        memset(hdr->name, 0, sizeof(hdr->name));
        strncpy(hdr->name, layer->name, sizeof(hdr->name) - 1);

        size_t blen = sizeof(layer_hdr_t);
        if (layer->binding_count > 0) {
            size_t bbytes = layer->binding_count * sizeof(key_binding_t);
            memcpy(buf + sizeof(layer_hdr_t), layer->bindings, bbytes);
            blen += bbytes;
        }

        if (s_shadow[i] && s_shadow_len[i] == blen &&
            memcmp(s_shadow[i], buf, blen) == 0) {
            continue;  // byte-for-byte what is already on flash
        }

        char key[8];
        layer_key(key, sizeof(key), i);
        if (nvs_set_blob(h, key, buf, blen) != ESP_OK) {
            app_log("KEYMAP", "NVS write failed for %s", key);
            ok = false;
            continue;
        }
        if (s_shadow[i]) {
            memcpy(s_shadow[i], buf, blen);
            s_shadow_len[i] = blen;
        }
        written++;
    }

    switch_blob_t *sb = (switch_blob_t *)buf;
    sb->magic = SWITCH_BLOB_MAGIC;
    sb->count = (uint8_t)engine->switch_map_count;
    memset(sb->reserved, 0, sizeof(sb->reserved));
    if (engine->switch_map_count > 0) {
        memcpy(sb->entries, engine->switch_map,
               engine->switch_map_count * sizeof(key_switch_map_entry_t));
    }
    if (!(s_switch_shadow && s_switch_shadow_len == sizeof(switch_blob_t) &&
          memcmp(s_switch_shadow, sb, sizeof(switch_blob_t)) == 0)) {
        if (nvs_set_blob(h, SWITCH_NVS_KEY, sb, sizeof(switch_blob_t)) != ESP_OK) {
            app_log("KEYMAP", "NVS write failed for %s", SWITCH_NVS_KEY);
            ok = false;
        } else {
            if (s_switch_shadow) {
                memcpy(s_switch_shadow, sb, sizeof(switch_blob_t));
                s_switch_shadow_len = sizeof(switch_blob_t);
            }
            written++;
        }
    }

    voice_blob_t vb;
    vb.magic = VOICE_BLOB_MAGIC;
    vb.trigger = engine->voice.trigger;
    vb.hotkey_mode = engine->voice.hotkey_mode;
    vb.max_sec = engine->voice.max_sec;
    if (!(s_voice_shadow_valid && memcmp(&s_voice_shadow, &vb, sizeof(vb)) == 0)) {
        if (nvs_set_blob(h, VOICE_NVS_KEY, &vb, sizeof(vb)) != ESP_OK) {
            app_log("KEYMAP", "NVS write failed for %s", VOICE_NVS_KEY);
            ok = false;
        } else {
            s_voice_shadow = vb;
            s_voice_shadow_valid = true;
            written++;
        }
    }

    // "active" is deliberately not written here either: the selected layer is
    // runtime state, so nothing in this file records it.
    key_engine_unlock();

    // Nothing changed: skip the commit entirely. A commit is what actually
    // programs a flash page, so this is the difference between "no wear" and
    // "one worn page" for a save that had nothing to record.
    if (written == 0) {
        nvs_close(h);
        heap_caps_free(buf);
        return true;
    }

    if (ok && nvs_commit(h) != ESP_OK) {
        app_log("KEYMAP", "NVS commit failed");
        ok = false;
    }
    nvs_close(h);
    heap_caps_free(buf);

    if (ok) {
        app_log("KEYMAP", "Keymap saved to NVS (%d key(s) changed)", written);
    }
    return ok;
}

bool key_config_storage_load(key_mapper_engine_t *engine)
{
    if (!engine) return false;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(LAYER_BLOB_MAX, MALLOC_CAP_SPIRAM);
    key_layer_t *tmp = (key_layer_t *)heap_caps_malloc(sizeof(key_layer_t) * MAX_LAYERS,
                                                       MALLOC_CAP_SPIRAM);
    if (!buf || !tmp) {
        if (buf) heap_caps_free(buf);
        if (tmp) heap_caps_free(tmp);
        key_engine_load_defaults(engine);
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(KEYMAP_NS, NVS_READONLY, &h) != ESP_OK) {
        heap_caps_free(buf);
        heap_caps_free(tmp);
        key_engine_load_defaults(engine);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < MAX_LAYERS && ok; i++) {
        char key[8];
        layer_key(key, sizeof(key), i);
        size_t len = LAYER_BLOB_MAX;
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len < sizeof(layer_hdr_t)) {
            ok = false;
            break;
        }
        layer_hdr_t *hdr = (layer_hdr_t *)buf;
        if (hdr->magic != LAYER_BLOB_MAGIC || hdr->binding_count > MAX_KEY_BINDINGS) {
            ok = false;
            break;
        }
        key_layer_t *layer = &tmp[i];
        memset(layer, 0, sizeof(*layer));
        strncpy(layer->name, hdr->name, sizeof(layer->name) - 1);
        layer->type = (layer_type_t)hdr->type;
        layer->timeout_sec = hdr->timeout_sec;
        layer->led_color = hdr->led_color;
        layer->binding_count = hdr->binding_count;
        if (hdr->binding_count > 0) {
            memcpy(layer->bindings, buf + sizeof(layer_hdr_t),
                   hdr->binding_count * sizeof(key_binding_t));
        }
    }

    // Migrate legacy factory layer types to persistent. The WebUI presents
    // configurations as long-lived profiles; the old default "15 s timeout"
    // and "one-shot" behaviours made a selected configuration silently revert.
    if (ok) {
        for (int i = 1; i < MAX_LAYERS; i++) {
            if ((tmp[i].type == LAYER_TYPE_TIMEOUT && tmp[i].timeout_sec == 15) ||
                tmp[i].type == LAYER_TYPE_ONESHOT) {
                tmp[i].type = LAYER_TYPE_PERSISTENT;
                tmp[i].timeout_sec = 0;
            }
        }
    }

    // The active layer is deliberately NOT restored. Which layer is selected is
    // runtime state, not configuration, and persisting it meant a flash write
    // every time the user switched layers - which the switch map turns into a
    // routine keypress. A device always starts on layer 0; "active" is only
    // erased below, to clear a value left by an older firmware.
    uint8_t active = 0;

    key_switch_map_entry_t smap_load[MAX_SWITCH_MAP];
    size_t smap_load_n = 0;
    bool smap_load_ok = false;
    {
        switch_blob_t sb;
        size_t slen = sizeof(sb);
        if (nvs_get_blob(h, SWITCH_NVS_KEY, &sb, &slen) == ESP_OK &&
            slen >= sizeof(switch_blob_t) && sb.magic == SWITCH_BLOB_MAGIC &&
            sb.count <= MAX_SWITCH_MAP) {
            memcpy(smap_load, sb.entries, sb.count * sizeof(key_switch_map_entry_t));
            smap_load_n = sb.count;
            smap_load_ok = true;
        }
    }
    key_voice_config_t voice_load;
    bool voice_load_ok = false;
    {
        voice_blob_t vb;
        size_t vlen = sizeof(vb);
        if (nvs_get_blob(h, VOICE_NVS_KEY, &vb, &vlen) == ESP_OK &&
            vlen == sizeof(vb) && vb.magic == VOICE_BLOB_MAGIC) {
            voice_load.trigger = vb.trigger;
            voice_load.hotkey_mode = vb.hotkey_mode;
            voice_load.max_sec = vb.max_sec;
            voice_config_sanitize(&voice_load);
            voice_load_ok = true;
            s_voice_shadow = vb;
            s_voice_shadow_valid = true;
        }
    }
    nvs_close(h);

    if (!ok) {
        heap_caps_free(buf);
        heap_caps_free(tmp);
        app_log("KEYMAP", "Stored keymap missing/invalid, using defaults");
        key_engine_load_defaults(engine);
        if (voice_load_ok) {
            key_engine_lock();
            engine->voice = voice_load;
            key_engine_unlock();
        }
        return false;
    }

    key_engine_lock();
    memcpy(engine->layers, tmp, sizeof(key_layer_t) * MAX_LAYERS);
    engine->layer_count = MAX_LAYERS;
    engine->active_layer = active;
    memset(engine->states, 0, sizeof(engine->states));
    if (smap_load_ok) {
        memcpy(engine->switch_map, smap_load, smap_load_n * sizeof(key_switch_map_entry_t));
        engine->switch_map_count = smap_load_n;
    }
    if (voice_load_ok) {
        engine->voice = voice_load;
    }
    engine->config_rev++;
    uint32_t color = engine->layers[active].led_color;
    key_engine_unlock();

    heap_caps_free(buf);
    heap_caps_free(tmp);
    led_indicator_set_layer_color(color);
    return true;
}

// Deferred save: the caller responds first, then this task writes to flash
// ~150 ms later so the NVS flash operation does not stall the USB reply. The
// delay also coalesces a burst of requests (the config page renames one button
// per request) into a single write.
static void keymap_save_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(150));
            key_config_storage_save(&g_key_engine);
        }
    }
}

// Only configuration changes are ever routed here. Anything that is runtime
// state - which layer is selected, which key is held - stays in RAM and never
// reaches flash.
void key_config_storage_request_save(void)
{
    if (s_save_sem) {
        xSemaphoreGive(s_save_sem);
    }
}

void key_config_storage_init(key_mapper_engine_t *engine)
{
    if (!s_save_sem) {
        s_save_sem = xSemaphoreCreateBinary();
        xTaskCreate(keymap_save_task, "keymap_save", 4096, NULL, 3, NULL);
    }
    if (key_config_storage_load(engine)) {
        app_log("KEYMAP", "Loaded custom keymap from NVS");
    } else {
        app_log("KEYMAP", "Loaded factory default keymap");
    }
}

void key_config_storage_reset_defaults(key_mapper_engine_t *engine)
{
    if (!engine) return;
    for (int i = 0; i < MAX_LAYERS; i++) {
        char key[8];
        layer_key(key, sizeof(key), i);
        config_store_erase_key(KEYMAP_NS, key);
    }
    config_store_erase_key(KEYMAP_NS, "active");
    config_store_erase_key(KEYMAP_NS, SWITCH_NVS_KEY);
    config_store_erase_key(KEYMAP_NS, VOICE_NVS_KEY);
    shadow_invalidate();

    key_engine_lock();
    key_engine_load_defaults(engine);
    uint32_t color = engine->layers[engine->active_layer].led_color;
    key_engine_unlock();

    led_indicator_set_layer_color(color);
    app_log("KEYMAP", "Keymap reset to factory defaults");
}

size_t key_telemetry_to_json(const key_mapper_engine_t *engine, char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    if (!engine) return 0;

    JsonDocument doc;
    doc["source_vk"] = engine->last_telemetry.source_vk;
    doc["is_pressed"] = engine->last_telemetry.is_pressed;
    doc["pressed_vk"] = key_engine_get_pressed_vk(engine);
    doc["duration_ms"] = engine->last_telemetry.duration_ms;
    doc["action_type"] = engine->last_telemetry.action_type;
    doc["modifier"] = engine->last_telemetry.modifier;
    doc["key_code"] = engine->last_telemetry.key_code;
    doc["consumer_code"] = engine->last_telemetry.consumer_code;
    doc["active_layer"] = engine->active_layer;
    doc["switch_mode"] = engine->switch_mode_active;

    return serializeJson(doc, out, out_len);
}
