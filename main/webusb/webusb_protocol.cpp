#include "webusb_protocol.h"
#include "app_config.h"
#include "version.h"
#include "app_log.h"
#include "ble/ble_remote_client.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_config_storage.h"
#include "keymap/key_names.h"
#include "audio/audio_pipeline.h"
#include "storage/config_store.h"
#include "usb/usb_composite.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ArduinoJson.h>

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern key_mapper_engine_t g_key_engine;

// Streaming keymap upload buffer (chunked save avoids large bulk OUT transfers).
static uint8_t *s_save_buf = NULL;
static size_t   s_save_len = 0;

static size_t copy_str(uint8_t *resp, size_t resp_cap, const char *src)
{
    if (!src) {
        src = "";
    }
    size_t len = strlen(src);
    if (len > resp_cap) {
        len = resp_cap;
    }
    memcpy(resp, src, len);
    return len;
}

static size_t ok(uint8_t *resp, size_t resp_cap, const char *json)
{
    return copy_str(resp, resp_cap, json);
}

static void schedule_restart(void)
{
    xTaskCreate([](void *) {
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
    }, "reboot", 2048, NULL, 3, NULL);
}

size_t webusb_protocol_handle(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                              uint8_t *resp, size_t resp_cap, uint8_t *status)
{
    if (!resp || resp_cap == 0) {
        if (status) *status = WEBUSB_ERR_INTERNAL;
        return 0;
    }
    *status = WEBUSB_OK;

    switch (cmd) {
        case CMD_DEVICE_INFO: {
            size_t w = (size_t)snprintf((char *)resp, resp_cap,
                "{\"name\":\"%s\",\"version\":\"%s\",\"build\":\"%s\",\"hardware\":\"%s\",\"protocol\":1,"
                "\"capabilities\":[\"keymap\",\"layers\",\"ble\",\"webusb\",\"uac\",\"hid\"]}",
                FIRMWARE_NAME, FIRMWARE_VERSION, FIRMWARE_BUILD, HARDWARE_TARGET);
            return w;
        }

        case CMD_STATUS: {
            size_t w = (size_t)snprintf((char *)resp, resp_cap,
                "{\"firmware\":\"%s\",\"version\":\"%s\",\"build\":\"%s\",\"uptime_sec\":%llu,"
                "\"ble_state\":%d,\"active_layer\":%u,\"battery\":%d,\"frames_decoded\":%u,"
                "\"samples_pushed\":%u,\"free_heap\":%u,\"free_psram\":%u,"
                "\"usb_mounted\":%s,\"switch_mode\":%s,\"config_rev\":%u,\"layout\":\"%s\","
                "\"reports_seen\":%u}",
                FIRMWARE_NAME, FIRMWARE_VERSION, FIRMWARE_BUILD,
                (unsigned long long)(esp_timer_get_time() / 1000000),
                (int)ble_remote_get_state(),
                (unsigned)key_engine_get_active_layer(&g_key_engine),
                ble_remote_get_battery(),
                (unsigned)g_audio_pipeline.total_frames_decoded,
                (unsigned)g_audio_pipeline.total_samples_pushed,
                (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                usb_composite_is_mounted() ? "true" : "false",
                key_engine_switch_mode_active(&g_key_engine) ? "true" : "false",
                (unsigned)g_key_engine.config_rev,
                ble_remote_layout_name(),
                (unsigned)ble_remote_reports_seen());
            return w;
        }

        case CMD_LOGS_GET:
            return app_log_get_json((char *)resp, resp_cap);

        case CMD_LOGS_CLEAR:
            app_log_clear();
            return ok(resp, resp_cap, "{\"status\":\"cleared\"}");

        case CMD_KEYMAP_GET:
            return key_config_to_json(&g_key_engine, (char *)resp, resp_cap);

        case CMD_KEYMAP_BEGIN: {
            if (!s_save_buf) {
                s_save_buf = (uint8_t *)heap_caps_malloc(WEBUSB_MAX_PAYLOAD + 1, MALLOC_CAP_SPIRAM);
            }
            if (!s_save_buf) {
                *status = WEBUSB_ERR_INTERNAL;
                return ok(resp, resp_cap, "{\"error\":\"no_mem\"}");
            }
            s_save_len = 0;
            return ok(resp, resp_cap, "{\"status\":\"begin\"}");
        }

        case CMD_KEYMAP_DATA: {
            if (!s_save_buf) {
                *status = WEBUSB_ERR_INTERNAL;
                return ok(resp, resp_cap, "{\"error\":\"no_begin\"}");
            }
            if (s_save_len + payload_len > WEBUSB_MAX_PAYLOAD) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"too_long\"}");
            }
            if (payload_len) {
                memcpy(s_save_buf + s_save_len, payload, payload_len);
                s_save_len += payload_len;
            }
            return ok(resp, resp_cap, "{\"status\":\"ok\"}");
        }

        case CMD_KEYMAP_COMMIT: {
            if (!s_save_buf || s_save_len == 0) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"empty\"}");
            }
            s_save_buf[s_save_len] = '\0';
            app_log("KEYMAP", "Saving keymap (%u bytes)", (unsigned)s_save_len);
            bool parsed = key_config_from_json(&g_key_engine, (char *)s_save_buf);
            s_save_len = 0;
            if (!parsed) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"invalid_keymap_format\"}");
            }
            key_config_storage_request_save();
            return ok(resp, resp_cap, "{\"status\":\"saved\"}");
        }

        case CMD_SET_LAYER: {
            if (!payload || payload_len == 0) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"missing_body\"}");
            }
            char *json = (char *)heap_caps_malloc(payload_len + 1, MALLOC_CAP_SPIRAM);
            if (!json) {
                *status = WEBUSB_ERR_INTERNAL;
                return ok(resp, resp_cap, "{\"error\":\"no_mem\"}");
            }
            memcpy(json, payload, payload_len);
            json[payload_len] = '\0';
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, json);
            heap_caps_free(json);
            if (err) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"invalid_json\"}");
            }
            uint8_t layer = doc["layer"] | 0;
            if (layer >= MAX_LAYERS) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"bad_layer\"}");
            }
            key_engine_lock();
            key_engine_switch_layer(&g_key_engine, layer, (uint32_t)(esp_timer_get_time() / 1000));
            key_engine_unlock();
            return ok(resp, resp_cap, "{\"status\":\"ok\"}");
        }

        case CMD_KEYMAP_SAVE: {
            if (!payload || payload_len == 0) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"missing_body\"}");
            }
            char *json = (char *)heap_caps_malloc(payload_len + 1, MALLOC_CAP_SPIRAM);
            if (!json) {
                *status = WEBUSB_ERR_INTERNAL;
                return ok(resp, resp_cap, "{\"error\":\"no_mem\"}");
            }
            memcpy(json, payload, payload_len);
            json[payload_len] = '\0';

            app_log("KEYMAP", "Saving keymap (%u bytes)", (unsigned)payload_len);
            bool parsed = key_config_from_json(&g_key_engine, json);
            heap_caps_free(json);
            if (!parsed) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"invalid_keymap_format\"}");
            }
            // Persist asynchronously so the flash write does not stall the reply.
            key_config_storage_request_save();
            return ok(resp, resp_cap, "{\"status\":\"saved\"}");
        }

        case CMD_KEYMAP_RESET:
            key_config_storage_reset_defaults(&g_key_engine);
            return ok(resp, resp_cap, "{\"status\":\"reset_ok\"}");

        case CMD_KEYMAP_TELEMETRY:
            return key_telemetry_to_json(&g_key_engine, (char *)resp, resp_cap);

        case CMD_BLE_SCAN:
            return ble_remote_scan_devices_json((char *)resp, resp_cap);

        case CMD_BLE_CONNECT: {
            if (!payload || payload_len == 0) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"missing_body\"}");
            }
            char *json = (char *)heap_caps_malloc(payload_len + 1, MALLOC_CAP_SPIRAM);
            if (!json) {
                *status = WEBUSB_ERR_INTERNAL;
                return ok(resp, resp_cap, "{\"error\":\"no_mem\"}");
            }
            memcpy(json, payload, payload_len);
            json[payload_len] = '\0';

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, json);
            heap_caps_free(json);
            if (err) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"invalid_json\"}");
            }
            const char *mac = doc["mac"] | "";
            const char *name = doc["name"] | "Xiaomi Voice Remote";
            uint8_t type = doc["type"] | 1;
            if (!mac[0]) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"empty_mac\"}");
            }
            bool ok_connect = ble_remote_connect_target(mac, type, name);
            return ok(resp, resp_cap, ok_connect ? "{\"status\":\"ok\"}" : "{\"status\":\"failed\"}");
        }

        case CMD_BLE_UNPAIR:
            ble_remote_unpair();
            return ok(resp, resp_cap, "{\"status\":\"ok\"}");

        case CMD_BLE_INFO:
            return ble_remote_get_connected_info((char *)resp, resp_cap);

        case CMD_BLE_RECONNECT:
            ble_remote_trigger_reconnect();
            return ok(resp, resp_cap, "{\"status\":\"reconnecting\"}");

        case CMD_BLE_RAW_REPORT: {
            bool on = (payload_len > 0) ? (payload[0] != 0) : true;
            ble_remote_set_raw_report_log(on);
            return ok(resp, resp_cap, on ? "{\"raw_report\":true}" : "{\"raw_report\":false}");
        }

        case CMD_BLE_LAYOUT: {
            // Payload is either a bare layout name or {"layout":"..."}.
            char name[24] = {0};
            if (payload_len > 0) {
                char raw[96] = {0};
                size_t n = payload_len < sizeof(raw) - 1 ? payload_len : sizeof(raw) - 1;
                memcpy(raw, payload, n);
                raw[n] = '\0';

                const char *value = raw;
                if (raw[0] == '{') {
                    JsonDocument doc;
                    if (deserializeJson(doc, raw) == DeserializationError::Ok &&
                        doc["layout"].is<const char *>()) {
                        value = doc["layout"].as<const char *>();
                    } else {
                        value = "";
                    }
                }
                strncpy(name, value ? value : "", sizeof(name) - 1);
            }
            ble_remote_set_layout_override(name);
            char out[64];
            snprintf(out, sizeof(out), "{\"layout\":\"%s\"}", ble_remote_layout_name());
            return ok(resp, resp_cap, out);
        }

        case CMD_KEY_NAMES_GET:
            return key_names_to_json((char *)resp, resp_cap);

        case CMD_KEY_NAMES_SET: {
            // Payload is {"slot":<0..15>,"name":"<utf8>"}. A name is the only
            // thing a client can set here, and an empty one clears the slot back
            // to its built-in label. The response is always the whole table, so
            // the caller never has to guess what the result was.
            char raw[256] = {0};
            if (payload_len == 0 || payload_len >= sizeof(raw)) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"bad_payload\"}");
            }
            memcpy(raw, payload, payload_len);
            raw[payload_len] = '\0';

            JsonDocument doc;
            if (deserializeJson(doc, raw) != DeserializationError::Ok ||
                !doc["slot"].is<int>()) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"bad_payload\"}");
            }

            int slot = doc["slot"].as<int>();
            const char *name = doc["name"].is<const char *>() ? doc["name"].as<const char *>() : "";
            if (!key_names_set(slot, name)) {
                *status = WEBUSB_ERR_ARG;
                return ok(resp, resp_cap, "{\"error\":\"bad_slot\"}");
            }
            return key_names_to_json((char *)resp, resp_cap);
        }

        case CMD_NVS_RESET:
            app_log("SYSTEM", "Factory reset requested, erasing NVS...");
            config_store_erase_all();
            schedule_restart();
            return ok(resp, resp_cap, "{\"status\":\"erased\",\"message\":\"NVS erased, rebooting\"}");

        case CMD_SYSTEM_RESTART:
            app_log("SYSTEM", "Reboot requested via WebUSB");
            schedule_restart();
            return ok(resp, resp_cap, "{\"status\":\"rebooting\"}");

        default:
            *status = WEBUSB_ERR_CMD;
            return ok(resp, resp_cap, "{\"error\":\"unknown_command\"}");
    }
}
