#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// WebUSB command opcodes (mirrors the old REST API surface).
enum {
    CMD_DEVICE_INFO      = 0x50,
    CMD_STATUS           = 0x01,
    CMD_LOGS_GET         = 0x02,
    CMD_LOGS_CLEAR       = 0x03,
    CMD_KEYMAP_GET       = 0x10,
    CMD_KEYMAP_SAVE      = 0x11,
    CMD_KEYMAP_RESET     = 0x12,
    CMD_KEYMAP_TELEMETRY = 0x13,
    CMD_KEYMAP_BEGIN     = 0x15,
    CMD_KEYMAP_DATA      = 0x16,
    CMD_KEYMAP_COMMIT    = 0x17,
    CMD_SET_LAYER        = 0x18,
    CMD_BLE_SCAN         = 0x20,
    CMD_BLE_CONNECT      = 0x21,
    CMD_BLE_UNPAIR       = 0x22,
    CMD_BLE_INFO         = 0x23,
    CMD_BLE_RECONNECT    = 0x24,
    CMD_BLE_RAW_REPORT   = 0x25,
    CMD_BLE_LAYOUT       = 0x26,
    CMD_KEY_NAMES_GET    = 0x27,
    CMD_KEY_NAMES_SET    = 0x28,
    CMD_NVS_RESET        = 0x31,
    CMD_SYSTEM_RESTART   = 0x40,
};

// Response status codes.
enum {
    WEBUSB_OK            = 0,
    WEBUSB_ERR_CMD       = 1,
    WEBUSB_ERR_ARG       = 2,
    WEBUSB_ERR_INTERNAL  = 3,
};

/**
 * @brief Execute a WebUSB command.
 *
 * @param cmd          Command opcode.
 * @param payload      Request payload (may be NULL).
 * @param payload_len  Payload length.
 * @param resp         Response buffer (never NULL).
 * @param resp_cap     Response buffer capacity.
 * @param status       Out: response status code.
 * @return Number of response bytes written (excluding any NUL terminator).
 */
size_t webusb_protocol_handle(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                              uint8_t *resp, size_t resp_cap, uint8_t *status);

#ifdef __cplusplus
}
#endif
