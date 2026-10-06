#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_STATE_DISCONNECTED = 0,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_TALKING
} ble_remote_state_t;

/** @brief Bring up the NimBLE host and start scanning for the remote. */
void ble_remote_init(void);

/** @brief Periodic state machine tick (scan restart, reconnect supervision). */
void ble_remote_task(void);

ble_remote_state_t ble_remote_get_state(void);

/** @brief Force a disconnect + rescan. */
void ble_remote_trigger_reconnect(void);

/** @brief Serialize the recently discovered BLE devices as JSON. */
size_t ble_remote_scan_devices_json(char *out, size_t out_len);

/** @brief Connect and bond to a specific device. */
bool ble_remote_connect_target(const char *mac_str, uint8_t addr_type, const char *dev_name);

/** @brief Connect to a MAC, looking up the address type from the scan cache. */
bool ble_remote_connect_mac(const char *mac_str);

/** @brief Delete all bonds and the saved remote. */
void ble_remote_unpair(void);

/** @brief Serialize connection state + bound remote info as JSON. */
size_t ble_remote_get_connected_info(char *out, size_t out_len);

/** @brief Remote battery level in percent (0-100), or -1 if unknown. */
int ble_remote_get_battery(void);

/**
 * @brief Log the next few HID notifications verbatim.
 *
 * Diagnostic aid for a remote whose report format is unknown: the raw bytes
 * are printed as hex so a new layout can be decoded from the device log.
 */
void ble_remote_set_raw_report_log(bool on);

/**
 * @brief Force the HID report dialect instead of detecting it from the name.
 *
 * @param layout  "auto", "rc003"/"xiaomi", or "google_tv"/"google".
 */
void ble_remote_set_layout_override(const char *layout);

/** @brief Active report dialect: "rc003", "google_tv" or "unknown". */
const char *ble_remote_layout_name(void);

/** @brief HID notifications parsed since boot (diagnostic). */
uint32_t ble_remote_reports_seen(void);

#ifdef __cplusplus
}
#endif
