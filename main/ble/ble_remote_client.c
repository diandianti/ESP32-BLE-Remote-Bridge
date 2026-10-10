#include "ble_remote_client.h"
#include "app_config.h"
#include "app_log.h"
#include "audio/atvv_audio.h"
#include "audio/audio_pipeline.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_names.h"
#include "led/led_indicator.h"
#include "storage/config_store.h"
#include "usb/hid_bridge.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

// Provided by the NimBLE store configuration but not declared in a public header.
void ble_store_config_init(void);

// ===========================================================================
// UUIDs
// ===========================================================================
static const ble_uuid128_t s_uuid_atvv_svc =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_cmd =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x02, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_aud =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x03, 0x00, 0x5e, 0xab);
static const ble_uuid128_t s_uuid_atvv_ctl =
    BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                     0x05, 0x4f, 0x21, 0x5a, 0x04, 0x00, 0x5e, 0xab);
static const ble_uuid16_t s_uuid_hogp_svc = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_uuid_hid_report = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t s_uuid_hid_proto_mode = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t s_uuid_hid_ctrl_point = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t s_uuid_batt_level = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t s_uuid_batt_status = BLE_UUID16_INIT(0x2BED);
static const ble_uuid16_t s_uuid_model_number = BLE_UUID16_INIT(0x2A24);

// ===========================================================================
// State
// ===========================================================================
static ble_remote_state_t s_state = BLE_STATE_DISCONNECTED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  s_own_addr_type = 0;

// Discovered characteristic handles
static uint16_t s_atvv_cmd_chr = 0;
static uint16_t s_atvv_aud_chr = 0;
static uint16_t s_atvv_ctl_chr = 0;
#define MAX_REPORTS 6
static uint16_t s_hid_report_chrs[MAX_REPORTS];
static int s_hid_report_count = 0;
static uint16_t s_hid_proto_chr = 0;
static uint16_t s_hid_ctrl_chr = 0;
static uint16_t s_batt_level_chr = 0;
static uint16_t s_batt_status_chr = 0;
static uint16_t s_model_chr = 0;
static bool     s_model_read = false;
static int      s_battery_level = -1;
static uint32_t s_battery_last_ms = 0;

// How often to re-read the remote's battery level while connected. Kept slow
// to limit extra BLE traffic and power draw.
#define BATTERY_REFRESH_INTERVAL_MS 900000  // 15 minutes
static uint16_t s_atvv_start = 0, s_atvv_end = 0;
static uint16_t s_hid_start = 0, s_hid_end = 0;

static size_t s_frame_size = AUDIO_DEFAULT_FRAME_BYTES;
static uint8_t s_session_id = 0;
static uint32_t s_conn_start_ms = 0;
static uint32_t s_talking_start_ms = 0;
static uint32_t s_mic_extend_last_ms = 0;
static bool s_discovery_started = false;

// The remote closes the microphone itself (AUDIO_STOP) in the normal case.
// A fixed 10 s cap used to back that up, and it cut every longer dictation off
// mid-sentence. The backstop is now "the audio stopped arriving": a session
// whose AUDIO_STOP was lost goes quiet, a live one never does.
// The length limit is user-configurable (voice.max_sec, 0 = none); it is what
// ends a toggled session whose second key press was missed.
#define VOICE_AUDIO_IDLE_MAX_MS  2000

// Bound remote (persisted in NVS)
static char    s_bound_mac[18] = {0};
static char    s_bound_name[40] = {0};
static uint8_t s_bound_type = 1;

// Connected remote info
static char s_connected_mac[18] = {0};
static char s_connected_name[40] = {0};

// Asynchronous requests from other tasks
static volatile bool s_req_unpair = false;
static volatile bool s_req_reconnect = false;
static volatile bool s_do_connect = false;
static char     s_pending_mac[18] = {0};
static uint8_t  s_pending_type = 1;
static char     s_pending_name[40] = {0};

// HOGP pressed-usage tracking
static uint16_t s_pressed[8];
static int      s_pressed_count = 0;

// Discovered device cache
typedef struct {
    char     name[40];
    char     mac[18];
    int8_t   rssi;
    uint8_t  type;
    uint32_t last_seen_ms;
} discovered_t;
static discovered_t s_discovered[BLE_MAX_DISCOVERED];
static int s_discovered_count = 0;
static portMUX_TYPE s_disc_mux = portMUX_INITIALIZER_UNLOCKED;

extern key_mapper_engine_t g_key_engine;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool mac_equals(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'F') ca += 32;
        if (cb >= 'A' && cb <= 'F') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

// Which remote family is connected; see "Remote layout" below.
typedef enum {
    REMOTE_LAYOUT_UNKNOWN = 0,
    REMOTE_LAYOUT_RC003,
    REMOTE_LAYOUT_GOOGLE_TV,
} remote_layout_t;

static remote_layout_t s_layout = REMOTE_LAYOUT_UNKNOWN;

// ===========================================================================
// Write operation queue (chained GATT procedures)
// ===========================================================================
typedef enum {
    PHASE_IDLE = 0,
    PHASE_SUBSCRIBE,
    PHASE_HID_INIT,
    PHASE_HANDSHAKE
} init_phase_t;

#define MAX_OPS 10
typedef struct {
    uint16_t handle;
    uint8_t  len;
    uint8_t  data[8];
} gatt_op_t;

static gatt_op_t s_ops[MAX_OPS];
static int s_op_count = 0;
static int s_op_idx = 0;
static init_phase_t s_phase = PHASE_IDLE;

static void run_next_op(void);
static uint16_t cccd_lookup(uint16_t chr_val_handle);
static void read_battery(void);
static void read_model_number(void);

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status != 0) {
        // 0x100 | BLE_HS_ENOTSUP is what NimBLE reports for a Write Without
        // Response once the value has been handed to the controller: there is
        // no ATT response by definition, so the local completion status is
        // "not supported" rather than an error from the peer. Treating it as a
        // failure is misleading - the writes to the ATVV command
        // characteristic, which is exactly this kind of write, demonstrably
        // reach the remote (pressing the voice button makes it stream audio).
        uint8_t host_status = (uint8_t)(error->status & 0xFF);
        if (error->status == (int)(0x100 | 8) || host_status == 8) {
            app_log("BLE", "write 0x%04X sent without response", s_ops[s_op_idx].handle);
        } else {
            app_log("BLE", "GATT write handle 0x%04X failed: %d", s_ops[s_op_idx].handle, error->status);
        }
    }
    s_op_idx++;
    run_next_op();
    return 0;
}

static void run_next_op(void)
{
    if (s_op_idx >= s_op_count) {
        // Phase complete -> advance to the next phase.
        if (s_phase == PHASE_HID_INIT) {
            s_phase = PHASE_SUBSCRIBE;
            s_op_count = 0;
            s_op_idx = 0;

            // ATVV audio + control (simple characteristics, CCCD = value + 1).
            uint16_t atvv[2] = { s_atvv_aud_chr, s_atvv_ctl_chr };
            for (int i = 0; i < 2; i++) {
                uint16_t chr = atvv[i];
                if (!chr) continue;
                uint16_t cccd = cccd_lookup(chr);
                if (!cccd) cccd = chr + 1;
                s_ops[s_op_count].handle = cccd;
                s_ops[s_op_count].data[0] = 0x01; // notifications enabled
                s_ops[s_op_count].data[1] = 0x00;
                s_ops[s_op_count].len = 2;
                s_op_count++;
                app_log("BLE", "Subscribe ATVV chr 0x%04X via CCCD 0x%04X", chr, cccd);
            }

            // HID input reports: use the discovered CCCD; output-only reports
            // have none and are skipped.
            for (int i = 0; i < s_hid_report_count && s_op_count < MAX_OPS; i++) {
                uint16_t chr = s_hid_report_chrs[i];
                uint16_t cccd = cccd_lookup(chr);
                if (!cccd) {
                    app_log("BLE", "Report chr 0x%04X has no CCCD, skipping", chr);
                    continue;
                }
                s_ops[s_op_count].handle = cccd;
                s_ops[s_op_count].data[0] = 0x01;
                s_ops[s_op_count].data[1] = 0x00;
                s_ops[s_op_count].len = 2;
                s_op_count++;
                app_log("BLE", "Subscribe HID report chr 0x%04X via CCCD 0x%04X", chr, cccd);
            }

            run_next_op();
        } else if (s_phase == PHASE_SUBSCRIBE) {
            s_phase = PHASE_HANDSHAKE;
            s_op_count = 0;
            s_op_idx = 0;
            if (s_atvv_cmd_chr) {
                // Google remote: GET_CAPS v1.0 - version(2) = 1.0, legacy
                // constant 0x0003, interaction models (1): 0x01 press-to-talk
                // + on-request for the toggle trigger, 0x03 adding hold-to-
                // talk for the hold trigger (see handle_atvv_ctl()). The RC003
                // keeps the v0.4 request it has always been sent.
                uint8_t caps_v10[6] = { 0x0A, 0x01, 0x00, 0x00, 0x03, 0x01 };
                static const uint8_t caps_v04[5] = { 0x0A, 0x00, 0x04, 0x00, 0x07 };
                if (key_engine_get_voice_config(&g_key_engine).trigger == VOICE_TRIGGER_HOLD) {
                    caps_v10[5] = 0x03;
                }
                bool google = (s_layout == REMOTE_LAYOUT_GOOGLE_TV);
                s_ops[s_op_count].handle = s_atvv_cmd_chr;
                memcpy(s_ops[s_op_count].data, google ? caps_v10 : caps_v04,
                       google ? sizeof(caps_v10) : sizeof(caps_v04));
                s_ops[s_op_count].len = google ? sizeof(caps_v10) : sizeof(caps_v04);
                s_op_count++;
            }
            run_next_op();
        } else if (s_phase == PHASE_HANDSHAKE) {
            s_phase = PHASE_IDLE;
            s_state = BLE_STATE_CONNECTED;
            led_indicator_set(LED_STATE_CONNECTED);
            app_log("BLE", "Remote ready: HOGP + ATVV initialized");
            read_model_number();
        }
        return;
    }

    gatt_op_t *op = &s_ops[s_op_idx];
    if (op->handle == s_atvv_cmd_chr) {
        // The ATVV command characteristic rejects a Write Request with ATT
        // 0x03 (write not permitted, logged as 259): it only accepts Write
        // Without Response, so GET_CAPS never reached the remote before.
        int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, op->handle, op->data, op->len);
        app_log("ATVV", "GET_CAPS sent (no response) rc=%d", rc);
        s_op_idx++;
        run_next_op();
        return;
    }
    int rc = ble_gattc_write_flat(s_conn_handle, op->handle, op->data, op->len, write_cb, NULL);
    if (rc != 0) {
        app_log("BLE", "ble_gattc_write_flat(0x%04X) rc=%d", op->handle, rc);
        s_op_idx++;
        run_next_op();
    }
}

// Attribute handles discovered from the peer's database.
#define MAX_CCCDS      32
#define MAX_ALL_CHRS   64
static uint16_t s_cccd_handles[MAX_CCCDS];
static int s_cccd_count = 0;
static uint8_t s_dsc_phase = 0;
static uint16_t s_all_chrs[MAX_ALL_CHRS];
static int s_all_chr_count = 0;

static uint16_t next_chr_handle(uint16_t chr)
{
    uint16_t best = 0;
    for (int i = 0; i < s_all_chr_count; i++) {
        uint16_t h = s_all_chrs[i];
        if (h > chr && (best == 0 || h < best)) {
            best = h;
        }
    }
    return best ? best : 0xFFFF;
}

// The CCCD of a characteristic is the first 0x2902 descriptor after its value
// handle and before the next characteristic. NimBLE reports chr_val_handle=0
// for a whole-range descriptor scan, so we cannot key the table by it; instead
// match by handle order. This handles characteristics (e.g. HOGP reports) that
// have a Report Reference descriptor before the CCCD.
static uint16_t cccd_lookup(uint16_t chr)
{
    uint16_t limit = next_chr_handle(chr);
    uint16_t best = 0;
    for (int i = 0; i < s_cccd_count; i++) {
        uint16_t h = s_cccd_handles[i];
        if (h > chr && h < limit && (best == 0 || h < best)) {
            best = h;
        }
    }
    return best;
}

// Start the post-discovery init sequence: HID Control Point / Protocol Mode
// first, then subscribe to notifications, then the ATVV GET_CAPS handshake.
static void begin_init_sequence(void)
{
    s_op_count = 0;
    s_op_idx = 0;
    s_phase = PHASE_HID_INIT;

    if (s_hid_ctrl_chr) {
        s_ops[s_op_count].handle = s_hid_ctrl_chr;
        s_ops[s_op_count].data[0] = 0x00; // exit suspend
        s_ops[s_op_count].len = 1;
        s_op_count++;
    }
    if (s_hid_proto_chr) {
        s_ops[s_op_count].handle = s_hid_proto_chr;
        s_ops[s_op_count].data[0] = 0x01; // report protocol mode
        s_ops[s_op_count].len = 1;
        s_op_count++;
    }

    run_next_op();
}

// Enumerate every descriptor so each notify characteristic can be mapped to
// its real Client Characteristic Configuration Descriptor (0x2902). Some
// characteristics (e.g. the HOGP report) have extra descriptors before the
// CCCD, so chr+1 is not reliable.
static int dsc_all_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (error->status == 0 && dsc) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            if (s_cccd_count < MAX_CCCDS) {
                s_cccd_handles[s_cccd_count++] = dsc->handle;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        // Chain the HOGP range after the ATVV range so we never scan all
        // 0xFFFF handles (which took ~11 s on a remote with high slave latency).
        if (s_dsc_phase == 0 && s_hid_start && s_hid_end >= s_hid_start) {
            s_dsc_phase = 1;
            int rc = ble_gattc_disc_all_dscs(conn_handle, s_hid_start, s_hid_end, dsc_all_cb, NULL);
            if (rc == 0) return 0;
            app_log("BLE", "disc HOGP descriptors rc=%d", rc);
        }
        app_log("BLE", "Discovered %d CCCD(s)", s_cccd_count);
        begin_init_sequence();
    }
    return 0;
}

// ===========================================================================
// Characteristic discovery
// ===========================================================================
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr) {
        if (s_all_chr_count < MAX_ALL_CHRS) {
            s_all_chrs[s_all_chr_count++] = chr->val_handle;
        }
        const ble_uuid_t *u = &chr->uuid.u;
        if (u->type == BLE_UUID_TYPE_128) {
            if (ble_uuid_cmp(u, &s_uuid_atvv_cmd.u) == 0) s_atvv_cmd_chr = chr->val_handle;
            else if (ble_uuid_cmp(u, &s_uuid_atvv_aud.u) == 0) s_atvv_aud_chr = chr->val_handle;
            else if (ble_uuid_cmp(u, &s_uuid_atvv_ctl.u) == 0) s_atvv_ctl_chr = chr->val_handle;
        } else if (u->type == BLE_UUID_TYPE_16) {
            if (ble_uuid_cmp(u, &s_uuid_hid_report.u) == 0) {
                // Only input reports notify; output reports have no CCCD.
                if ((chr->properties & 0x30) && s_hid_report_count < MAX_REPORTS) {
                    s_hid_report_chrs[s_hid_report_count++] = chr->val_handle;
                }
            } else if (ble_uuid_cmp(u, &s_uuid_hid_proto_mode.u) == 0) {
                s_hid_proto_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_hid_ctrl_point.u) == 0) {
                s_hid_ctrl_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_batt_level.u) == 0) {
                s_batt_level_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_batt_status.u) == 0) {
                s_batt_status_chr = chr->val_handle;
            } else if (ble_uuid_cmp(u, &s_uuid_model_number.u) == 0) {
                s_model_chr = chr->val_handle;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        app_log("BLE", "Chars: cmd=0x%04X aud=0x%04X ctl=0x%04X reports=%d proto=0x%04X cpoint=0x%04X",
                s_atvv_cmd_chr, s_atvv_aud_chr, s_atvv_ctl_chr,
                s_hid_report_count, s_hid_proto_chr, s_hid_ctrl_chr);
        s_cccd_count = 0;
        s_dsc_phase = 0;
        int rc = -1;
        if (s_atvv_start && s_atvv_end >= s_atvv_start) {
            rc = ble_gattc_disc_all_dscs(conn_handle, s_atvv_start, s_atvv_end, dsc_all_cb, NULL);
        } else if (s_hid_start && s_hid_end >= s_hid_start) {
            s_dsc_phase = 1;
            rc = ble_gattc_disc_all_dscs(conn_handle, s_hid_start, s_hid_end, dsc_all_cb, NULL);
        }
        if (rc != 0) {
            app_log("BLE", "disc_all_dscs rc=%d, subscribing with fallback handles", rc);
            begin_init_sequence();
        }
    }
    return 0;
}

// ===========================================================================
// Service discovery
// ===========================================================================
static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error->status == 0 && svc) {
        const ble_uuid_t *u = &svc->uuid.u;
        if (u->type == BLE_UUID_TYPE_128 && ble_uuid_cmp(u, &s_uuid_atvv_svc.u) == 0) {
            s_atvv_start = svc->start_handle;
            s_atvv_end = svc->end_handle;
            app_log("BLE", "ATVV service: 0x%04X-0x%04X", s_atvv_start, s_atvv_end);
        } else if (u->type == BLE_UUID_TYPE_16 && ble_uuid_cmp(u, &s_uuid_hogp_svc.u) == 0) {
            s_hid_start = svc->start_handle;
            s_hid_end = svc->end_handle;
            app_log("BLE", "HOGP service: 0x%04X-0x%04X", s_hid_start, s_hid_end);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ble_gattc_disc_all_chrs(conn_handle, 1, 0xFFFF, chr_disc_cb, NULL);
    }
    return 0;
}

static void start_discovery(void)
{
    if (s_discovery_started || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    s_discovery_started = true;
    s_atvv_cmd_chr = s_atvv_aud_chr = s_atvv_ctl_chr = 0;
    s_hid_proto_chr = s_hid_ctrl_chr = 0;
    s_hid_report_count = 0;
    s_model_chr = 0;
    s_model_read = false;
    s_atvv_start = s_atvv_end = s_hid_start = s_hid_end = 0;
    s_cccd_count = 0;
    s_all_chr_count = 0;
    app_log("BLE", "Discovering GATT services...");
    ble_gattc_disc_all_svcs(s_conn_handle, svc_disc_cb, NULL);
}

// ===========================================================================
// Notification handling
// ===========================================================================
// Defined further down with the other ATVV helpers.
static void atvv_audio_note_frame(uint16_t len);
static void atvv_audio_reset_stats(void);
static void atvv_audio_log_stats(void);
static void atvv_send_mic_open(void);
static void atvv_send_mic_close(void);
// Arms the raw-audio hex trace; defined with the dump state further down.
static void atvv_audio_arm_dump(void);

// Set by ble_remote_set_raw_report_log(); declared here because the ATVV
// control handler and the HID handlers both consult it.
static bool s_raw_report_log;

// ATVV control-characteristic opcodes and reason codes (Voice over BLE v1.0).
#define ATVV_CTL_AUDIO_STOP       0x00
#define ATVV_CTL_AUDIO_START      0x04
#define ATVV_CTL_START_SEARCH     0x08
#define ATVV_CTL_MIC_OPEN_ERROR   0x0C
#define ATVV_START_PTT            0x01
#define ATVV_START_HTT            0x03
#define ATVV_STOP_HTT_RELEASE     0x02
#define ATVV_STOP_UPCOMING_START  0x04

// Stream id 0 is the host-owned stream our MIC_OPEN started; ids 0x01..0x80
// are streams the remote started for a button press.
#define ATVV_STREAM_HOST          0x00
// The remote ends any stream after 15 s with AUDIO_STOP(0x02). A 0x02 that
// comes sooner on the host-owned stream is the voice key being pressed.
#define ATVV_REMOTE_STREAM_LIMIT_MS 14000
// A press that ends the host stream may be followed by its own button
// AUDIO_START; one that arrives this soon after is the same press, not a new
// one. Short, so a deliberate quick re-press still starts a new session.
#define ATVV_STOP_PRESS_GUARD_MS  500

// Set while the MIC_OPEN that continues a hold-to-talk session is in flight.
static bool s_continue_pending = false;
// When the current stream (not session) started.
static uint32_t s_stream_start_ms = 0;
// Until when a button AUDIO_START belongs to the press that just stopped us.
static uint32_t s_stop_guard_until_ms = 0;

// End the current voice session: release the voice action, and tell the
// remote to stop encoding unless it already has.
static void voice_session_end(bool send_close)
{
    s_state = BLE_STATE_CONNECTED;
    s_continue_pending = false;
    atvv_audio_log_stats();
    if (send_close) atvv_send_mic_close();
    key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, false, now_ms());
    app_log("ATVV", "Voice stop after %lu ms", (unsigned long)(now_ms() - s_talking_start_ms));
}

static void handle_atvv_ctl(const uint8_t *data, size_t len)
{
    if (len < 1) return;
    uint8_t op = data[0];

    // Control messages are rare (start, stop, one sync per frame is excluded
    // below), so log every one: the stop reason is what tells a key release
    // apart from the remote's own session timeout.
    if (op != 0x0A) {
        char hex[3 * 16 + 1];
        size_t n = (len < 16) ? len : 16;
        for (size_t i = 0; i < n; i++) snprintf(&hex[i * 3], 4, "%02X ", data[i]);
        hex[n * 3] = '\0';
        app_log("ATVV", "ctl rx len=%u [%s]", (unsigned)len, hex);
    }

    // Voice is press-to-start, press-to-stop. The remote's hold-to-talk mode
    // cannot be used: the ZTKA-IR57 reports "button released" (AUDIO_STOP
    // reason 0x02) after exactly 15 s with the button still held, and
    // MIC_EXTEND does not move that limit. GET_CAPS therefore asks for
    // press-to-talk, where the session lasts until the host's MIC_CLOSE and
    // MIC_EXTEND keeps it alive. START_SEARCH (on-request remotes) is handled
    // the same way: first press opens the microphone, the next one closes it.
    //
    // A remote that ignores the request and stays in hold-to-talk still works:
    // its "button released" stop is answered with MIC_OPEN, which turns the
    // session into a host-owned stream that only MIC_CLOSE ends.
    //
    // The RC003 keeps its original behaviour: its voice key is a HID key that
    // opens the microphone itself, so only a button-triggered AUDIO_START is
    // treated as a voice press there, and START_SEARCH is not acted on.
    //
    // The user can pick hold-to-talk instead (voice.trigger); then a button
    // stop ends the session, and the remote's own 15 s cap applies.
    const bool google = (s_layout == REMOTE_LAYOUT_GOOGLE_TV);
    const bool toggle = google &&
        key_engine_get_voice_config(&g_key_engine).trigger == VOICE_TRIGGER_TOGGLE;
    if (op == ATVV_CTL_AUDIO_START && len >= 2) {
        uint8_t reason = data[1];
        uint8_t codec = (len >= 3) ? data[2] : 0;
        bool by_button = (reason == ATVV_START_PTT || reason == ATVV_START_HTT);
        uint8_t stream = (len >= 4) ? data[3] : 0;
        if (!google && reason != ATVV_START_HTT) return;
        if (s_state == BLE_STATE_TALKING) {
            // The reply to our own MIC_OPEN carries stream id 0 (the remote
            // labels it reason 0x01, so the reason alone cannot tell). A
            // button stream (id != 0) appearing once the first button stream
            // has ended - while we own the stream, or while our MIC_OPEN is
            // still in flight - is the voice key pressed again.
            if (by_button && stream != ATVV_STREAM_HOST &&
                (s_session_id == ATVV_STREAM_HOST || s_continue_pending)) {
                s_session_id = stream;
                app_log("ATVV", "Voice key pressed again -> stop");
                voice_session_end(true);
                return;
            }
            if (stream == ATVV_STREAM_HOST) s_continue_pending = false;
            s_session_id = stream;
            s_stream_start_ms = now_ms();
            atvv_audio_begin_session();
            app_log("ATVV", "Voice stream continued (stream %u)", s_session_id);
            return;
        }
        if (toggle && stream == ATVV_STREAM_HOST) {
            // A late reply to the MIC_OPEN of a session that has since been
            // stopped. It is never a key press; close it again.
            s_session_id = stream;
            atvv_send_mic_close();
            app_log("ATVV", "Closing late host stream after stop");
            return;
        }
        if (toggle && by_button && (int32_t)(now_ms() - s_stop_guard_until_ms) < 0) {
            s_session_id = stream;
            atvv_send_mic_close();
            app_log("ATVV", "Ignoring stream %u from the stop press", stream);
            return;
        }
        s_session_id = stream;
        s_stream_start_ms = now_ms();
        s_state = BLE_STATE_TALKING;
        s_talking_start_ms = now_ms();
        s_mic_extend_last_ms = s_talking_start_ms;
        atvv_audio_reset_stats();
        atvv_audio_begin_session();
        atvv_audio_arm_dump();
        key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, true, now_ms());
        app_log("ATVV", "Voice start (reason %u, stream %u, codec %u)", reason, s_session_id, codec);
    } else if (op == ATVV_CTL_AUDIO_STOP) {
        uint8_t reason = (len >= 2) ? data[1] : 0;
        if (s_state != BLE_STATE_TALKING || reason == ATVV_STOP_UPCOMING_START) {
            // Idle, or a new AUDIO_START follows immediately.
            return;
        }
        if (toggle && reason == ATVV_STOP_HTT_RELEASE &&
            s_session_id == ATVV_STREAM_HOST && !s_continue_pending &&
            (int32_t)(now_ms() - s_stream_start_ms) < ATVV_REMOTE_STREAM_LIMIT_MS) {
            // Our own stream stopped early: the voice key was pressed.
            app_log("ATVV", "Voice key pressed again -> stop");
            s_stop_guard_until_ms = now_ms() + ATVV_STOP_PRESS_GUARD_MS;
            voice_session_end(false);
            return;
        }
        if (toggle && reason == ATVV_STOP_HTT_RELEASE) {
            // The button stream ended (key released after the first press),
            // or the remote's 15 s limit hit: carry on with a host stream.
            app_log("ATVV", "Remote ended hold-to-talk -> continuing with MIC_OPEN");
            s_continue_pending = true;
            atvv_send_mic_open();
            return;
        }
        app_log("ATVV", "Remote stopped audio (reason 0x%02X)", reason);
        // MIC_CLOSE as before: a remote that already stopped ignores it.
        voice_session_end(true);
    } else if (op == ATVV_CTL_START_SEARCH) {
        if (s_state == BLE_STATE_TALKING) {
            app_log("ATVV", "Voice key pressed again -> stop");
            voice_session_end(true);
        } else if (google) {
            atvv_send_mic_open();
        }
    } else if (op == ATVV_CTL_MIC_OPEN_ERROR && len >= 3) {
        app_log("ATVV", "MIC_OPEN_ERROR 0x%04X", (unsigned)((data[1] << 8) | data[2]));
        if (s_state == BLE_STATE_TALKING && s_continue_pending) {
            voice_session_end(false);  // the continuation was refused
        }
    } else if (op == 0x0B && len >= 7) {
        uint16_t ver = (uint16_t)((data[1] << 8) | data[2]);
        uint8_t codecs = (len >= 4) ? data[3] : 0;
        uint16_t fs = (uint16_t)((data[5] << 8) | data[6]);
        if (fs > 0) s_frame_size = fs;
        app_log("ATVV", "Capabilities: ver=0x%04X codecs=0x%02X frame=%u",
                ver, codecs, (unsigned)s_frame_size);
        if (len <= 16) {
            char hex[3 * 16 + 1];
            for (size_t i = 0; i < len; i++) {
                snprintf(&hex[i * 3], 4, "%02X ", data[i]);
            }
            hex[len * 3] = '\0';
            app_log("ATVV", "caps raw [%s]", hex);
        }
    } else if (op == 0x0A && len >= 7) {
        int16_t pred = (int16_t)((data[4] << 8) | data[5]);
        int8_t step = (int8_t)data[6];
        audio_pipeline_sync(&g_audio_pipeline, pred, step);
    }
}

// ATVV MIC_OPEN: opcode 0x0C followed by a 2-byte codec_used field
// (0x0001 = ADPCM 8 kHz/16-bit, 0x0002 = ADPCM 16 kHz/16-bit). Without the
// codec field a conforming remote cannot tell which stream to produce.
//
// The requested codec has to match what the rest of the firmware is built for.
// It used to ask for 16 kHz unconditionally while AUDIO_SAMPLE_RATE - and with
// it the decoder's expectations, the UAC descriptor and the isochronous packet
// size - were all 8 kHz. This remote happens to ignore the request and send
// codec 1 anyway, so nothing broke; a remote that honoured it would have been
// decoded and played back at half speed. Deriving it removes that trap.
#if AUDIO_SAMPLE_RATE >= 16000
#define ATVV_MIC_CODEC 0x0002u
#define ATVV_MIC_CODEC_NAME "ADPCM 16 kHz"
#else
#define ATVV_MIC_CODEC 0x0001u
#define ATVV_MIC_CODEC_NAME "ADPCM 8 kHz"
#endif

static void atvv_send_mic_open(void)
{
    if (!s_atvv_cmd_chr || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    // The Google remote speaks v1.0, which reads byte 1 as the mic mode
    // (0x00 = playback, realtime); v0.4 reads a big-endian codec, so one
    // layout serves both. The RC003 keeps the bytes it has always been sent.
    uint8_t cmd[3] = { 0x0C, (uint8_t)(ATVV_MIC_CODEC & 0xFF), (uint8_t)(ATVV_MIC_CODEC >> 8) };
    if (s_layout == REMOTE_LAYOUT_GOOGLE_TV) {
        cmd[1] = (uint8_t)(ATVV_MIC_CODEC >> 8);
        cmd[2] = (uint8_t)(ATVV_MIC_CODEC & 0xFF);
    }
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_atvv_cmd_chr, cmd, sizeof(cmd));
    app_log("ATVV", "MIC_OPEN (codec 0x%04X = %s) rc=%d",
            (unsigned)ATVV_MIC_CODEC, ATVV_MIC_CODEC_NAME, rc);
}

// ATVV MIC_EXTEND: opcode 0x0E followed by the stream id from AUDIO_START.
// Resets the remote's Audio Transfer Timeout, which would otherwise end a
// press-to-talk or on-request session after 15 s - 1 min.
static void atvv_send_mic_extend(void)
{
    if (!s_atvv_cmd_chr || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t cmd[2] = { 0x0E, s_session_id };
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_atvv_cmd_chr, cmd, sizeof(cmd));
    app_log("ATVV", "MIC_EXTEND stream=%u rc=%d", (unsigned)s_session_id, rc);
}

// ATVV MIC_CLOSE: opcode 0x0D, no payload.
static void atvv_send_mic_close(void)
{
    if (!s_atvv_cmd_chr || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    // v1.0 carries the stream id; a v0.4 remote ignores the extra byte.
    uint8_t cmd[2] = { 0x0D, s_session_id };
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_atvv_cmd_chr, cmd, sizeof(cmd));
    app_log("ATVV", "MIC_CLOSE rc=%d", rc);
}

// ===========================================================================
// Remote layout
//
// The two supported remotes multiplex different things onto the same HID
// report characteristic, and their payloads are genuinely ambiguous: the
// Xiaomi RC003 packs 16-bit little-endian HID usages, while the Google TV
// Remote sends a one-byte button index, so a two-byte RC003 report `01 00`
// and a Google index report `[01]` are indistinguishable from the bytes
// alone. The layout is therefore decided once per connection from the
// advertised name, with a manual override for anything unusual.
// (remote_layout_t and s_layout are declared near the top of the file.)
// ===========================================================================

// Counts every HID notification that reaches the parser, so "the remote is not
// being heard at all" is distinguishable from "the report was not understood".
static uint32_t s_reports_seen_total = 0;

// Counts only the notifications that carried a non-zero key slot, which is what
// the unconditional trace and the button table are keyed off.
static uint32_t s_reports_press_total = 0;

// Bounds the per-notification trace in the GAP handler.
static uint32_t s_notify_trace = 0;

// Per-path call counters. These make it unambiguous whether a report reached
// the Google index parser or was handed to the RC003 decoder, which is the one
// thing the aggregate counter cannot show.
static uint32_t s_gtv_calls = 0;
static uint32_t s_rc003_calls = 0;
static uint32_t s_release_calls = 0;

// State for the periodic health line printed by ble_remote_task().
static uint32_t s_health_last_ms = 0;
static uint32_t s_health_last_reports = 0;

static remote_layout_t s_layout_override = REMOTE_LAYOUT_UNKNOWN;

const char *ble_remote_layout_name(void)
{
    switch (s_layout) {
        case REMOTE_LAYOUT_RC003:     return "rc003";
        case REMOTE_LAYOUT_GOOGLE_TV: return "google_tv";
        default:                      return "unknown";
    }
}

uint32_t ble_remote_reports_seen(void)
{
    return s_reports_seen_total;
}
void ble_remote_set_layout_override(const char *layout)
{
    if (!layout || !layout[0] || strcasecmp(layout, "auto") == 0) {
        s_layout_override = REMOTE_LAYOUT_UNKNOWN;
    } else if (strcasecmp(layout, "google_tv") == 0 || strcasecmp(layout, "google") == 0) {
        s_layout_override = REMOTE_LAYOUT_GOOGLE_TV;
    } else if (strcasecmp(layout, "rc003") == 0 || strcasecmp(layout, "xiaomi") == 0) {
        s_layout_override = REMOTE_LAYOUT_RC003;
    } else {
        s_layout_override = REMOTE_LAYOUT_UNKNOWN;
    }
    app_log("BLE", "Remote layout override: %s", layout && layout[0] ? layout : "auto");
}

static bool name_contains(const char *name, const char *needle)
{
    return name && needle && strstr(name, needle) != NULL;
}

static remote_layout_t detect_layout(const char *name)
{
    if (!name || !name[0]) return REMOTE_LAYOUT_UNKNOWN;
    // Google's remote advertises as "Google TV Remote"; match on the brand
    // only, so regional suffixes do not change the decision.
    if (name_contains(name, "Google") || name_contains(name, "google")) {
        return REMOTE_LAYOUT_GOOGLE_TV;
    }
    if (name_contains(name, "MI RC") || name_contains(name, "Xiaomi") ||
        name_contains(name, "xiaomi") || name_contains(name, "小米") ||
        name_contains(name, "遥控")) {
        return REMOTE_LAYOUT_RC003;
    }
    return REMOTE_LAYOUT_UNKNOWN;
}

// Choose the parser for this connection. Returns true when the layout changed.
static bool apply_layout_for_connection(const char *name)
{
    remote_layout_t want = s_layout_override;
    if (want == REMOTE_LAYOUT_UNKNOWN) {
        want = detect_layout(name);
    }
    if (want == REMOTE_LAYOUT_UNKNOWN) {
        // Fall back to the RC003 interpretation, which is what the firmware
        // has always done; the raw-report log will show if that was wrong.
        want = REMOTE_LAYOUT_RC003;
    }
    app_log("BLE", "Remote \"%s\" -> layout %s", name && name[0] ? name : "?",
            want == REMOTE_LAYOUT_GOOGLE_TV ? "google_tv" : "rc003");
    bool changed = (want != s_layout);
    s_layout = want;
    return changed;
}

// ===========================================================================
// Google TV Remote (G20BTS / G9N9N) HID report
//
// Unlike the RC003, which packs 16-bit HID keyboard usages into its report,
// the Google TV Remote sends a report ID followed by a one-byte *button
// index* (0x01..0x0D while held, 0x00 on release). None of those values are
// HID usages, which is why every button used to be dropped as an unknown key
// code. Observed layouts:
//
//   [01 00]                    report ID 1, all released
//   [01 NN]                    report ID 1, button index NN held
//   [01 NN 00]                 same with a trailing pad byte
//   [02 NN 00 00 00 00]        report ID 2, same index in byte 1
//   [NN 00] / [NN 00 00 00 00 00]  no report ID, index in byte 0
//
// The parser below accepts all of them and converts the index into the
// MI_KEY_GTV_* canonical codes defined in keymap/key_definitions.h.
// ===========================================================================
static uint32_t s_raw_report_count = 0;
#define RAW_REPORT_LOG_LIMIT 64

// Last report printed by the learn trace, so a held button - which repeats the
// same report - produces one line instead of a stream.
static uint8_t s_learn_last[16];
static size_t  s_learn_last_len = 0;
// Number of ATVV audio notifications to dump as hex while the raw log is on.
// 64 notifications is roughly 1.3 kB, which is what it takes to answer the
// questions the decoder cannot be asked on its own: whether the stream carries a
// per-block header (look for periodic plateaus and full-scale runs) and whether
// the remote's encoder restarts its predictor on every block. Eight was enough
// to recognise a codec, but less than one 134-byte ATVV frame - not enough to
// see any structure at all.
#define AUDIO_DUMP_FRAMES 64
// Notifications to skip before dumping. The first fraction of a second of a
// session is near-silence - the microphone has just been powered up and the
// user has not spoken yet - and a stream of near-zero nibbles says nothing about
// the block structure. One second in, the user is talking.
#define AUDIO_DUMP_SKIP 200
static int s_aud_dump_remaining = 0;
static int s_aud_dump_skip = 0;

// Sessions left that dump their raw audio automatically. Two is enough to see
// the stream structure twice over, and keeps the log readable afterwards: the
// alternative was asking the user to find and toggle "raw report" in the
// config site before every diagnostic run, which is exactly the kind of step
// that gets forgotten and then wastes a round trip.
static int s_dump_sessions_left = 2;

static void atvv_audio_arm_dump(void)
{
    if (s_dump_sessions_left > 0) {
        s_dump_sessions_left--;
        s_aud_dump_skip = AUDIO_DUMP_SKIP;
        s_aud_dump_remaining = AUDIO_DUMP_FRAMES;
    }
}

void ble_remote_set_raw_report_log(bool on)
{
    s_raw_report_log = on;
    s_raw_report_count = 0;
    s_learn_last_len = 0;
    s_aud_dump_skip = 0;                     // explicit request: dump immediately
    s_aud_dump_remaining = on ? AUDIO_DUMP_FRAMES : 0;
    app_log("HOGP", "raw report log %s", on ? "on" : "off");
}

static void handle_hid_release(void)
{
    if (s_pressed_count == 0) return;
    for (int i = 0; i < s_pressed_count; i++) {
        if (s_pressed[i] <= 0xFF) {
            key_engine_feed_key(&g_key_engine, (uint8_t)s_pressed[i], false, now_ms());
        }
    }
    s_pressed_count = 0;
}

// The remote exposes TWO HID report characteristics with *independent* index
// spaces, and that is the whole source of the "wrong button" and "several
// buttons trigger the assistant" reports:
//
//   main report   (2-byte `[idx, 0]`, chr handle 0x0021, entry 1 below)
//       idx 1..7  = up, down, left, right, select, back, home
//       idx 9..13 = mute, preset app 1, preset app 2, power, input
//       (idx 8 is not used by this remote)
//   second report (6-byte `[idx, 0, 0, 0, 0, 0]`, chr handle 0x001D, entry 0)
//       idx 1 = volume up, idx 2 = volume down
//
// Index 1 therefore means "up" in one report and "volume up" in the other, and
// index 2 means "down" in one and "volume down" in the other. Resolving both
// through a single index -> slot table made those pairs indistinguishable:
// pressing volume-up ran the *power* slot's action and pressing down ran the
// *assistant* slot's action. The index is only unique per report, so the report
// has to be part of the identity.
//
// The assistant button is deliberately absent: it sends NO HID report at all. It
// announces itself with an ATVV AUDIO_START on the control characteristic, which
// handle_atvv_ctl() already turns into a voice press - so it keeps working
// without an entry here, and adding one would create a second, competing path.
//
// Which characteristic is which is derived, not assumed: discovery subscribes
// 0x001D first and 0x0021 second, and every 2-byte button report in the captures
// arrives as `NOTIFY: handle=0x0021`. The 6-byte volume reports must therefore
// come from 0x001D, i.e. entry 0 of s_hid_report_chrs[].
#define GTV_REPORT_SECONDARY 0

// Map (report characteristic index, button index) to the canonical key code.
// Returns 0 for a combination this remote does not define; the caller logs the
// raw bytes then, so an unrecognised button is visible instead of silent.
//
// These codes are the *shared* ones the keymap stores, so each button lands on
// the same physical slot as it would on the Xiaomi remote - which is why the
// built-in slot names and the factory key map both line up without changes.
static uint8_t google_tv_index_to_key(int chr_index, int index)
{
    if (chr_index == GTV_REPORT_SECONDARY) {
        switch (index) {
            case 1:  return MI_KEY_VOL_UP;
            case 2:  return MI_KEY_VOL_DOWN;
            default: return 0;
        }
    }
    switch (index) {
        case 1:  return MI_KEY_UP;
        case 2:  return MI_KEY_DOWN;
        case 3:  return MI_KEY_LEFT;
        case 4:  return MI_KEY_RIGHT;
        case 5:  return MI_KEY_OK;
        case 6:  return MI_KEY_BACK;
        case 7:  return MI_KEY_HOME;
        case 9:  return MI_KEY_GTV_MUTE;
        case 10: return MI_KEY_GTV_APP_1;     // preset app (YouTube)
        case 11: return MI_KEY_GTV_APP_2;     // preset app (Netflix)
        case 12: return MI_KEY_POWER;
        case 13: return MI_KEY_TV;            // input / source select
        default: return 0;
    }
}

// Human-readable name for the canonical key codes this bridge decodes, used in
// the key log so a button press is identifiable without a lookup table.
//
// A user-defined name wins: the config page lets the user label each physical
// slot themselves, and the log is the one place where seeing their own label
// instead of a built-in guess is the whole point.
static const char *key_code_name(uint8_t key)
{
    const char *custom = key_names_get(key_engine_slot_for_vk(key));
    if (custom[0]) return custom;

    switch (key) {
        case MI_KEY_POWER:    return "power";
        case MI_KEY_VOICE:    return "voice";
        case MI_KEY_UP:       return "up";
        case MI_KEY_DOWN:     return "down";
        case MI_KEY_LEFT:     return "left";
        case MI_KEY_RIGHT:    return "right";
        case MI_KEY_OK:       return "ok";
        case MI_KEY_BACK:     return "back";
        case MI_KEY_HOME:     return "home";
        case MI_KEY_MENU:     return "menu";
        case MI_KEY_VOL_UP:   return "vol+";
        case MI_KEY_VOL_DOWN: return "vol-";
        case MI_KEY_TV:       return "input";
        case MI_KEY_GTV_MUTE: return "mute";
        case MI_KEY_GTV_APP_1:return "app1";
        case MI_KEY_GTV_APP_2:return "app2";
        default:              return "?";
    }
}

static void handle_google_tv_report(const uint8_t *data, size_t len, int chr_index)
{
    // Locate the button index: the first non-zero byte. Observed layouts are
    // `[01 NN]`, `[01 NN 00]`, `[02 NN 00 00 00 00]` and `[NN 00 ...]`, i.e.
    // either a report ID followed by the index or the index first, always with
    // zero padding. All-zero reports (releases) never reach this function.
    int index = -1;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            index = data[i];
            break;
        }
    }

    uint8_t new_key = 0;
    if (index > 0) {
        new_key = google_tv_index_to_key(chr_index, index);
        if (new_key == 0) {
            // Unknown button: state the report it came from and the raw bytes so
            // the table can be extended from a real capture instead of
            // guesswork. Rate limited.
            static uint32_t s_warned = 0;
            if (s_warned < 16) {
                s_warned++;
                app_log("HOGP", "chr#%d index %d (0x%02X) has no key mapping",
                        chr_index, index, index);
            }
            return;
        }
    }

    if (s_pressed_count > 0 && s_pressed[0] == new_key) {
        return;  // same button still held; the remote repeats the report
    }

    handle_hid_release();
    if (new_key == 0) return;

    s_pressed[0] = new_key;
    s_pressed_count = 1;
    app_log("HOGP", "button chr#%d idx=%d -> key 0x%02X (%s) DOWN", chr_index, index,
            new_key, key_code_name(new_key));

    // The ATVV microphone stream is opened by whichever button the *keymap* has
    // bound to the voice action, not by a fixed index. Keying it off a hard-coded
    // code meant the mic only ever opened for one particular button: move the
    // voice action to the key you actually press and voice input went dead while
    // the hotkey still fired. Reading the binding keeps the two together.
    key_binding_t binding = {0};
    bool wants_mic = false;
    if (key_engine_get_binding(&g_key_engine, new_key, &binding)) {
        wants_mic = (binding.has_click && binding.click_action.type == ACTION_VOICE_HOLD) ||
                    (binding.has_long && binding.long_action.type == ACTION_VOICE_HOLD);
    }
    if (wants_mic) {
        atvv_send_mic_open();
    }
    key_engine_feed_key(&g_key_engine, new_key, true, now_ms());
}

static void handle_hid_report(const uint8_t *data, size_t len, int chr_index)
{
    if (len == 0) return;

    s_reports_seen_total++;

    // An all-zero report means "all keys released". Every byte must be checked,
    // including byte 0: this remote reports `[index, 0x00]`, so the index lives
    // in byte 0 and byte 1 is the zero padding. Skipping byte 0 here made every
    // button press look like a release and silently discarded it.
    bool any = false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) { any = true; break; }
    }
    if (!any) {
        s_release_calls++;
        s_learn_last_len = 0;  // so the next press of the same button is traced again
        handle_hid_release();
        return;
    }

    s_reports_press_total++;

    // Unconditional trace of the first notifications of a session. Deliberately
    // independent of every later decision: this is the record that shows what
    // the remote actually sends, so a parsing mistake cannot hide the evidence.
    if (s_reports_press_total <= 32) {
        char hex[3 * 24 + 1];
        size_t n = (len < 24) ? len : 24;
        for (size_t i = 0; i < n; i++) {
            snprintf(&hex[i * 3], 4, "%02X ", data[i]);
        }
        hex[n * 3] = '\0';
        app_log("HOGP", "rx#%d len=%u [%s]", chr_index, (unsigned)len, hex);
    }

    // Learn trace: while the raw-report log is on, print one line per *distinct*
    // report. The config page's button-learning panel reads these lines out of
    // the log, so exactly one line per press - and none for the repeated reports
    // a held button produces - is what makes "press a button, see which slot it
    // landed in" work without the firmware having to know what the button means.
    if (s_raw_report_log) {
        size_t n = (len < sizeof(s_learn_last)) ? len : sizeof(s_learn_last);
        if (n != s_learn_last_len || memcmp(s_learn_last, data, n) != 0) {
            memcpy(s_learn_last, data, n);
            s_learn_last_len = n;
            char hex[3 * sizeof(s_learn_last) + 1];
            for (size_t i = 0; i < n; i++) {
                snprintf(&hex[i * 3], 4, "%02X ", data[i]);
            }
            hex[n * 3] = '\0';
            app_log("LEARN", "chr#%d len=%u [%s]", chr_index, (unsigned)len, hex);
        }
    }

    // Dispatch on the report length rather than on the remote's advertised
    // name. The RC003 sends 7-byte (report ID + three 16-bit usages) and
    // occasional 8-byte boot-keyboard reports; the Google TV Remote sends a
    // single-byte button index in a 1..6 byte report. An explicit layout
    // override still forces one parser for a remote that breaks this rule.
    bool use_rc003 = (s_layout_override == REMOTE_LAYOUT_RC003) ||
                     (s_layout_override == REMOTE_LAYOUT_UNKNOWN && (len == 7 || len == 8));
    if (!use_rc003) {
        s_gtv_calls++;
        handle_google_tv_report(data, len, chr_index);
        return;
    }
    s_rc003_calls++;

    // Distinct keys that can be reported at once. The RC003 packs up to three
    // 16-bit usages; a boot keyboard report has six slots.
    uint16_t usages[6];
    int usage_count = 0;

    if (len >= 8) {
        // Classic boot keyboard report [modifier, reserved, k0..k5]. The
        // previous code only looked at k0, so a combination could never be
        // detected.
        for (size_t i = 2; i < len && usage_count < (int)(sizeof(usages) / sizeof(usages[0])); i++) {
            if (data[i] != 0) usages[usage_count++] = data[i];
        }
    } else {
        // RC003 HOGP report: leading report ID 1 followed by 16-bit
        // little-endian keyboard usages.
        const uint8_t *p = data;
        size_t n = len;
        if ((n % 2) == 1 && p[0] == 0x01) {
            p++;
            n--;
        }
        if ((n % 2) == 0) {
            for (size_t i = 0; i + 1 < n && usage_count < (int)(sizeof(usages) / sizeof(usages[0])); i += 2) {
                uint16_t u = (uint16_t)(p[i] | (p[i + 1] << 8));
                if (u != 0) usages[usage_count++] = u;
            }
        }
    }

    if (usage_count == 0) {
        // Nothing that looks like a HID usage. Log the bytes once so an
        // unrecognised report shape is visible instead of silently dropped.
        if (s_raw_report_log && s_raw_report_count < RAW_REPORT_LOG_LIMIT) {
            char hex[3 * 24 + 1];
            size_t n = (len < 24) ? len : 24;
            for (size_t i = 0; i < n; i++) snprintf(&hex[i * 3], 4, "%02X ", data[i]);
            hex[n * 3] = '\0';
            app_log("HOGP", "unparsed report len=%u [%s]", (unsigned)len, hex);
            s_raw_report_count++;
        }
        return;
    }

    // Diff against the previously pressed set.
    for (int i = 0; i < s_pressed_count; i++) {
        bool still = false;
        for (int j = 0; j < usage_count; j++) {
            if (usages[j] == s_pressed[i]) { still = true; break; }
        }
        if (!still && s_pressed[i] <= 0xFF) {
            key_engine_feed_key(&g_key_engine, (uint8_t)s_pressed[i], false, now_ms());
        }
    }
    for (int j = 0; j < usage_count; j++) {
        bool was = false;
        for (int i = 0; i < s_pressed_count; i++) {
            if (usages[j] == s_pressed[i]) { was = true; break; }
        }
        if (!was && usages[j] <= 0xFF) {
            app_log("HOGP", "Key usage 0x%02X DOWN", usages[j]);
            if (usages[j] == MI_KEY_VOICE_ALT || usages[j] == MI_KEY_VOICE) {
                atvv_send_mic_open();
            }
            key_engine_feed_key(&g_key_engine, (uint8_t)usages[j], true, now_ms());
        }
    }

    s_pressed_count = usage_count;
    for (int i = 0; i < usage_count; i++) {
        s_pressed[i] = usages[i];
    }
}

// ===========================================================================
// Battery level (Battery Service 0x180F, characteristic 0x2A19)
// ===========================================================================
static int battery_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status != 0) {
        app_log("BATTERY", "read failed: %d", error->status);
        return 0;
    }
    if (attr && attr->om) {
        uint8_t buf[8];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        if (os_mbuf_copydata(attr->om, 0, len, buf) == 0 && len >= 1) {
            s_battery_level = buf[0];
            if (s_battery_level > 100) s_battery_level = 100;
            app_log("BATTERY", "Remote battery: %d%%", s_battery_level);
        }
    }
    return 0;
}

static void read_battery(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_batt_level_chr == 0) {
        return;
    }
    s_battery_last_ms = now_ms();
    ble_gattc_read(s_conn_handle, s_batt_level_chr, battery_read_cb, NULL);
}

// ===========================================================================
// Device model (Device Information Service 0x180A, characteristic 0x2A24)
//
// RC001/RC003 firmware 2671 packs IMA-ADPCM high-nibble-first, while the ARN9
// firmware used by the Bluetooth Remote 2 / 2 Pro packs low-nibble-first.
// Reading the model string lets us pick the correct decode order; otherwise
// the voice stream decodes into noise.
// ===========================================================================
static int model_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status != 0) {
        app_log("MODEL", "read failed: %d", error->status);
        read_battery();
        return 0;
    }
    if (attr && attr->om) {
        char buf[48];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
        if (os_mbuf_copydata(attr->om, 0, len, buf) == 0) {
            buf[len] = '\0';
            app_log("MODEL", "Remote model: %s", buf);
            for (char *p = buf; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            }
            if (strstr(buf, "ARN9")) {
                audio_pipeline_set_nibble_order(&g_audio_pipeline, true);
                app_log("MODEL", "ARN9 detected -> ADPCM low-nibble-first");
            }
        }
    }
    read_battery();
    return 0;
}

static void read_model_number(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_model_chr == 0 || s_model_read) {
        read_battery();
        return;
    }
    s_model_read = true;
    int rc = ble_gattc_read(s_conn_handle, s_model_chr, model_read_cb, NULL);
    if (rc != 0) {
        app_log("MODEL", "ble_gattc_read rc=%d", rc);
        read_battery();
    }
}

// NimBLE (central) does not exchange the ATT MTU automatically. Without this
// the link stays at the 23-byte default and the remote can only send 20-byte
// audio notifications, which is far below the ~8 KB/s needed for 16 kHz ADPCM.
static int mtu_exchange_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           uint16_t mtu, void *arg)
{
    (void)conn_handle; (void)arg;
    app_log("BLE", "MTU exchange done: status=%d mtu=%u", error->status, mtu);
    return 0;
}

// ===========================================================================
// GAP event handler
// ===========================================================================
static void add_discovered(const ble_addr_t *addr, const char *name, int8_t rssi)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);

    portENTER_CRITICAL(&s_disc_mux);
    int found = -1;
    for (int i = 0; i < s_discovered_count; i++) {
        if (mac_equals(s_discovered[i].mac, mac)) { found = i; break; }
    }
    if (found < 0) {
        if (s_discovered_count >= BLE_MAX_DISCOVERED) {
            memmove(&s_discovered[0], &s_discovered[1], sizeof(discovered_t) * (BLE_MAX_DISCOVERED - 1));
            s_discovered_count = BLE_MAX_DISCOVERED - 1;
        }
        found = s_discovered_count++;
        memset(&s_discovered[found], 0, sizeof(discovered_t));
        strncpy(s_discovered[found].mac, mac, sizeof(s_discovered[found].mac) - 1);
    }
    if (name && name[0]) {
        strncpy(s_discovered[found].name, name, sizeof(s_discovered[found].name) - 1);
    }
    s_discovered[found].rssi = rssi;
    s_discovered[found].type = addr->type;
    s_discovered[found].last_seen_ms = now_ms();
    portEXIT_CRITICAL(&s_disc_mux);
}

static bool adv_is_target(const ble_addr_t *addr, const char *name, const uint8_t *data, uint8_t len)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);

    if (s_bound_mac[0]) {
        if (mac_equals(mac, s_bound_mac)) return true;
        if (s_bound_name[0] && name && name[0] && strcasecmp(name, s_bound_name) == 0) return true;
        return false;
    }

    if (name && (strstr(name, BLE_REMOTE_NAME_PREFIX) ||
                 strstr(name, BLE_REMOTE_NAME_EXTRA_1) ||
                 strstr(name, BLE_REMOTE_NAME_EXTRA_2) ||
                 strstr(name, "Xiaomi") || strstr(name, "Remote") ||
                 strstr(name, "小米") || strstr(name, "遥控"))) {
        return true;
    }

    // Look for the ATVV 128-bit service UUID (0xAB5E0001...) in the advertising data.
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t ad_len = data[i];
        if (ad_len == 0) break;
        uint8_t ad_type = data[i + 1];
        if ((ad_type == 0x06 || ad_type == 0x07) && ad_len >= 17) {
            // 128-bit UUID, little-endian encoded.
            static const uint8_t atvv_prefix[4] = { 0x64, 0xb6, 0x17, 0xf6 };
            if (memcmp(&data[i + 2], atvv_prefix, 4) == 0) return true;
        }
        i += ad_len + 1;
    }
    return false;
}

static void start_scan(void);
static void do_connect_addr(const ble_addr_t *addr);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_gap_disc_desc *d = &event->disc;
            char name[40] = {0};
            for (uint8_t i = 0; i + 1 < d->length_data; ) {
                uint8_t ad_len = d->data[i];
                if (ad_len == 0) break;
                uint8_t ad_type = d->data[i + 1];
                if ((ad_type == 0x08 || ad_type == 0x09) && ad_len >= 2) {
                    uint8_t n = ad_len - 1;
                    if (n > sizeof(name) - 1) n = sizeof(name) - 1;
                    memcpy(name, &d->data[i + 2], n);
                    name[n] = '\0';
                }
                i += ad_len + 1;
            }
            add_discovered(&d->addr, name, d->rssi);

            if (!s_do_connect && s_state <= BLE_STATE_SCANNING &&
                adv_is_target(&d->addr, name, d->data, d->length_data)) {
                app_log("BLE", "Target found: %s (%02x:%02x:%02x:%02x:%02x:%02x)",
                        name[0] ? name : "unknown",
                        d->addr.val[5], d->addr.val[4], d->addr.val[3],
                        d->addr.val[2], d->addr.val[1], d->addr.val[0]);
                ble_gap_disc_cancel();
                s_pending_type = d->addr.type;
                if (name[0]) {
                    strncpy(s_pending_name, name, sizeof(s_pending_name) - 1);
                }
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         d->addr.val[5], d->addr.val[4], d->addr.val[3],
                         d->addr.val[2], d->addr.val[1], d->addr.val[0]);
                strncpy(s_pending_mac, mac, sizeof(s_pending_mac) - 1);
                s_do_connect = true;
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (s_state == BLE_STATE_SCANNING) {
                s_state = BLE_STATE_DISCONNECTED;
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_state = BLE_STATE_CONNECTING;
                s_conn_start_ms = now_ms();
                s_discovery_started = false;
                app_log("BLE", "GATT connected (conn=%u)", s_conn_handle);
                struct ble_gap_conn_desc cdesc;
                if (ble_gap_conn_find(s_conn_handle, &cdesc) == 0) {
                    app_log("BLE", "Conn params: itvl=%u latency=%u timeout=%u",
                            cdesc.conn_itvl, cdesc.conn_latency, cdesc.supervision_timeout);
                }
                int mtu_rc = ble_gattc_exchange_mtu(s_conn_handle, mtu_exchange_cb, NULL);
                if (mtu_rc != 0) {
                    app_log("BLE", "MTU exchange failed to start: rc=%d", mtu_rc);
                }
                int rc = ble_gap_security_initiate(s_conn_handle);
                if (rc != 0) {
                    app_log("BLE", "security_initiate rc=%d, discovering anyway", rc);
                    start_discovery();
                }
            } else {
                app_log("BLE", "Connect failed: %d", event->connect.status);
                s_state = BLE_STATE_DISCONNECTED;
                start_scan();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            app_log("BLE", "Disconnected (reason=%d)", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_state = BLE_STATE_DISCONNECTED;
            s_discovery_started = false;
            s_pressed_count = 0;
            s_batt_level_chr = 0;
            s_batt_status_chr = 0;
            s_model_chr = 0;
            s_model_read = false;
            s_battery_level = -1;
            key_engine_release_all(&g_key_engine, now_ms());
            usb_hid_keyboard_release();
            usb_hid_consumer_release();
            usb_hid_mouse_buttons_release();
            audio_pipeline_stop_session(&g_audio_pipeline);
            led_indicator_set(LED_STATE_WAIT_CONNECTION);
            break;

        case BLE_GAP_EVENT_ENC_CHANGE: {            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                app_log("BLE", "Encryption change: status=%d encrypted=%d bonded=%d authenticated=%d",
                        event->enc_change.status, desc.sec_state.encrypted,
                        desc.sec_state.bonded, desc.sec_state.authenticated);
            } else {
                app_log("BLE", "Encryption change: status=%d", event->enc_change.status);
            }
            if (event->enc_change.status == 0 || !s_discovery_started) {
                start_discovery();
            }
            break;
        }

        case BLE_GAP_EVENT_MTU:
            app_log("BLE", "MTU updated: %u", event->mtu.value);
            break;

        case BLE_GAP_EVENT_CONN_UPDATE: {
            struct ble_gap_conn_desc udesc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &udesc) == 0) {
                app_log("BLE", "Conn updated: status=%d itvl=%u latency=%u timeout=%u",
                        event->conn_update.status, udesc.conn_itvl,
                        udesc.conn_latency, udesc.supervision_timeout);
            } else {
                app_log("BLE", "Conn updated: status=%d", event->conn_update.status);
            }
            break;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            struct os_mbuf *om = event->notify_rx.om;
            uint16_t handle = event->notify_rx.attr_handle;
            uint8_t buf[512];
            uint16_t len = OS_MBUF_PKTLEN(om);
            if (len > sizeof(buf)) len = sizeof(buf);
            if (os_mbuf_copydata(om, 0, len, buf) != 0) break;

            // Trace the first notifications of every kind, with the attribute
            // handle. This is the ground truth for "which characteristic is the
            // remote actually notifying on", which a missing subscription or a
            // mismatched handle would otherwise hide.
            if (s_notify_trace < 48 && handle != s_atvv_aud_chr) {
                s_notify_trace++;
                char hex[3 * 16 + 1];
                size_t n = (len < 16) ? len : 16;
                for (size_t i = 0; i < n; i++) snprintf(&hex[i * 3], 4, "%02X ", buf[i]);
                hex[n * 3] = '\0';
                app_log("NOTIFY", "handle=0x%04X len=%u [%s]", handle, (unsigned)len, hex);
            }

            if (handle == s_atvv_aud_chr) {
                if (s_aud_dump_skip > 0) {
                    s_aud_dump_skip--;
                } else if (s_aud_dump_remaining > 0) {
                    s_aud_dump_remaining--;
                    char hex[3 * 48 + 1];
                    size_t n = (len < 48) ? len : 48;
                    for (size_t i = 0; i < n; i++) {
                        snprintf(&hex[i * 3], 4, "%02X ", buf[i]);
                    }
                    hex[n * 3] = '\0';
                    app_log("ATVV", "audio len=%u [%s]", (unsigned)len, hex);
                }
                atvv_audio_note_frame(len);
                atvv_audio_feed(buf, len);
            } else if (handle == s_atvv_ctl_chr) {
                handle_atvv_ctl(buf, len);
            } else {
                for (int i = 0; i < s_hid_report_count; i++) {
                    if (handle == s_hid_report_chrs[i]) {
                        handle_hid_report(buf, len, i);
                        break;
                    }
                }
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

// ===========================================================================
// ATVV audio notification profile
//
// The RC003 streams 120-byte IMA-ADPCM frames (16 kHz), while the Google TV
// Remote was measured sending ~20-byte frames at ~12.5 Hz. Track the size
// histogram and the notification rate so the log states what the remote
// actually sends instead of assuming the Xiaomi numbers.
// ===========================================================================
static uint32_t s_aud_frame_total = 0;
static uint32_t s_aud_bytes_total = 0;
static uint32_t s_aud_last_ms = 0;
static uint32_t s_aud_min_ms = 0xFFFFFFFFu;
static uint32_t s_aud_max_ms = 0;
static uint16_t s_aud_len_min = 0xFFFF;
static uint16_t s_aud_len_max = 0;

// How far the decoder had drifted from the encoder by the end of each frame,
// measured against the next frame's sync word. Near zero means the header
// layout and nibble order are right; large values mean they are not.
static uint32_t s_sync_count = 0;
static uint32_t s_sync_pred_err_max = 0;
static uint32_t s_sync_step_err_max = 0;

static void atvv_audio_reset_stats(void)
{
    s_sync_count = 0;
    s_sync_pred_err_max = 0;
    s_sync_step_err_max = 0;
    s_aud_frame_total = 0;
    s_aud_bytes_total = 0;
    s_aud_last_ms = 0;
    s_aud_min_ms = 0xFFFFFFFFu;
    s_aud_max_ms = 0;
    s_aud_len_min = 0xFFFF;
    s_aud_len_max = 0;
}

static void atvv_audio_note_frame(uint16_t len)
{
    uint32_t now = now_ms();
    if (s_aud_last_ms != 0) {
        uint32_t dt = now - s_aud_last_ms;
        if (dt < s_aud_min_ms) s_aud_min_ms = dt;
        if (dt > s_aud_max_ms) s_aud_max_ms = dt;
    }
    s_aud_last_ms = now;
    s_aud_frame_total++;
    s_aud_bytes_total += len;
    if (len < s_aud_len_min) s_aud_len_min = len;
    if (len > s_aud_len_max) s_aud_len_max = len;
}

static void atvv_audio_log_stats(void)
{
    if (s_aud_frame_total == 0) {
        app_log("ATVV", "audio: no frames received");
        return;
    }
    atvv_audio_stats_t st;
    atvv_audio_get_stats(&st);
    uint32_t span = (s_aud_last_ms != 0 && s_aud_max_ms != 0)
                        ? (s_aud_last_ms - s_aud_max_ms)
                        : 0;
    app_log("ATVV", "audio: %lu packet(s) -> %lu frame(s), len %u..%u, interval %lu..%lu ms",
            (unsigned long)s_aud_frame_total, (unsigned long)st.frames,
            (unsigned)s_aud_len_min, (unsigned)s_aud_len_max,
            (unsigned long)((s_aud_min_ms == 0xFFFFFFFFu) ? 0 : s_aud_min_ms),
            (unsigned long)s_aud_max_ms);
    if (atvv_audio_seq_framed()) {
        app_log("ATVV", "seq framing: frames=%lu last_seq=%u pred=%d step=%d resync=%lu dropped=%lu",
                (unsigned long)st.frames, (unsigned)st.last_frame_number,
                st.last_predictor, st.last_step_index,
                (unsigned long)st.resyncs, (unsigned long)st.dropped_packets);
        app_log("ATVV", "seq sync drift: frames=%lu max_pred_err=%lu max_step_err=%lu",
                (unsigned long)s_sync_count, (unsigned long)s_sync_pred_err_max,
                (unsigned long)s_sync_step_err_max);
    } else if (atvv_audio_header_detected()) {
        app_log("ATVV", "ATVV framing: ver=0x%02X frame=%u codec=0x%02X resync=%lu dropped=%lu",
                st.version, (unsigned)st.last_frame_number, st.codec_bits,
                (unsigned long)st.resyncs, (unsigned long)st.dropped_packets);
        const char *rate = (st.codec_bits & 0x04) ? "16 kHz"
                           : (st.codec_bits & 0x02) ? "8 kHz" : "unspecified";
        app_log("ATVV", "ATVV codec: ADPCM %s (firmware built for %d Hz)",
                rate, AUDIO_SAMPLE_RATE);
    } else {
        app_log("ATVV", "bare payload stream (no ATVV header), span %lu ms",
                (unsigned long)span);
    }
}

// Called by the ATVV frame assembler with whole 128-byte payloads (conforming
// remote) or with raw notifications (bare-payload remote).
void atvv_audio_payload_ready(const uint8_t *payload, size_t len)
{
    audio_pipeline_feed_adpcm(&g_audio_pipeline, payload, len);
}

void atvv_audio_sync_ready(int16_t predictor, int8_t step_index)
{
    if (s_sync_count++ > 0) {
        int32_t dp = g_audio_pipeline.adpcm.predictor - predictor;
        int32_t ds = g_audio_pipeline.adpcm.step_index - step_index;
        if (dp < 0) dp = -dp;
        if (ds < 0) ds = -ds;
        if ((uint32_t)dp > s_sync_pred_err_max) s_sync_pred_err_max = (uint32_t)dp;
        if ((uint32_t)ds > s_sync_step_err_max) s_sync_step_err_max = (uint32_t)ds;
    }
    audio_pipeline_sync(&g_audio_pipeline, predictor, step_index);
}

// ===========================================================================
// Connect / scan control
// ===========================================================================
static struct ble_gap_conn_params s_conn_params = {
    .scan_itvl = 16,
    .scan_window = 16,
    .itvl_min = 6,       // 7.5 ms: enough events to carry 16 kHz ADPCM (~67 frames/s)
    .itvl_max = 12,      // 15 ms
    .latency = 0,
    .supervision_timeout = 400,
    .min_ce_len = 0,
    .max_ce_len = 0,
};

static void do_connect_addr(const ble_addr_t *addr)
{
    s_state = BLE_STATE_CONNECTING;
    led_indicator_set(LED_STATE_WAIT_CONNECTION);
    int rc = ble_gap_connect(s_own_addr_type, addr, 10000, &s_conn_params, gap_event_cb, NULL);
    if (rc != 0) {
        app_log("BLE", "ble_gap_connect rc=%d", rc);
        s_state = BLE_STATE_DISCONNECTED;
        start_scan();
    }
}

static void start_scan(void)
{
    if (s_state == BLE_STATE_CONNECTING || s_state >= BLE_STATE_CONNECTED) {
        return;
    }
    struct ble_gap_disc_params params = {0};
    params.itvl = (uint16_t)(BLE_SCAN_INTERVAL_MS * 1000 / 625);
    params.window = (uint16_t)(BLE_SCAN_WINDOW_MS * 1000 / 625);
    params.passive = 0;
    params.filter_duplicates = 0;
    params.limited = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == 0) {
        s_state = BLE_STATE_SCANNING;
        led_indicator_set(LED_STATE_WAIT_CONNECTION);
        app_log("BLE", "Scanning for Xiaomi remote...");
    } else if (rc != BLE_HS_EALREADY) {
        app_log("BLE", "ble_gap_disc rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        app_log("BLE", "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        app_log("BLE", "infer_auto rc=%d", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason)
{
    app_log("BLE", "Host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ===========================================================================
// Public API
// ===========================================================================
void ble_remote_init(void)
{
    size_t len = config_store_get_str("ble_conf", "bound_mac", s_bound_mac, sizeof(s_bound_mac));
    if (len > 0) {
        char typebuf[4] = {0};
        config_store_get_str("ble_conf", "bound_name", s_bound_name, sizeof(s_bound_name));
        config_store_get_str("ble_conf", "bound_type", typebuf, sizeof(typebuf));
        s_bound_type = (uint8_t)atoi(typebuf);
        app_log("BLE", "Loaded bound remote: %s (%s)", s_bound_name, s_bound_mac);
    }

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    ble_att_set_preferred_mtu(512);

    nimble_port_freertos_init(host_task);
    app_log("BLE", "NimBLE host started");
}

void ble_remote_task(void)
{
    uint32_t now = now_ms();

    // Handle asynchronous requests from the WebUSB task.
    if (s_req_unpair || s_req_reconnect) {
        bool is_unpair = s_req_unpair;
        s_req_unpair = false;
        s_req_reconnect = false;
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (is_unpair) {
            ble_store_clear();
            s_connected_mac[0] = '\0';
            s_connected_name[0] = '\0';
        }
        s_state = BLE_STATE_DISCONNECTED;
        start_scan();
    }

    if (s_do_connect) {
        s_do_connect = false;
        ble_gap_disc_cancel();
        ble_addr_t addr = {0};
        addr.type = s_pending_type;
        unsigned int v[6];
        if (sscanf(s_pending_mac, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            for (int i = 0; i < 6; i++) addr.val[5 - i] = (uint8_t)v[i];
            strncpy(s_connected_mac, s_pending_mac, sizeof(s_connected_mac) - 1);
            strncpy(s_connected_name, s_pending_name[0] ? s_pending_name : "Xiaomi Voice Remote",
                    sizeof(s_connected_name) - 1);
            // Pick the report parser before the first notification can arrive.
            apply_layout_for_connection(s_connected_name);
            do_connect_addr(&addr);
        }
    }

    // Fallback: if security never completes, still attempt discovery.
    if (s_state == BLE_STATE_CONNECTING && s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        !s_discovery_started && (now - s_conn_start_ms) > 3000) {
        app_log("BLE", "Security timeout -> discovering services anyway");
        start_discovery();
    }

    if (s_state == BLE_STATE_DISCONNECTED) {
        start_scan();
    }

    // Periodic reachability report. This is the only way to tell "the remote is
    // not sending notifications" apart from "the parser rejected them": the
    // counter increments for every notification that arrives on any subscribed
    // HID report characteristic, before any interpretation happens.
    if (s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_TALKING) {
        if ((now - s_health_last_ms) >= 5000) {
            s_health_last_ms = now;
            uint32_t reports = s_reports_seen_total - s_health_last_reports;
            s_health_last_reports = s_reports_seen_total;
            // nvs= is the NVS partition's used-entry count. It is the flash-wear
            // gauge: any write, from this firmware or from the NimBLE bond store,
            // moves it. A value that holds steady while the remote is used means
            // nothing is programming flash.
            uint32_t nvs_used = 0;
            config_store_get_usage(&nvs_used, NULL, NULL);
            app_log("HEALTH", "rx=%lu total=%lu release=%lu gtv=%lu rc003=%lu layout=%s nvs=%lu",
                    (unsigned long)reports, (unsigned long)s_reports_seen_total,
                    (unsigned long)s_release_calls, (unsigned long)s_gtv_calls,
                    (unsigned long)s_rc003_calls, ble_remote_layout_name(),
                    (unsigned long)nvs_used);
        }
    } else {
        s_health_last_ms = now;
        s_health_last_reports = s_reports_seen_total;
    }

    // Keep the remote's own session timer from ending a long dictation.
    if (s_state == BLE_STATE_TALKING &&
        (int32_t)(now - s_mic_extend_last_ms) >= BLE_KEEP_ALIVE_INTERVAL) {
        s_mic_extend_last_ms = now;
        atvv_send_mic_extend();
    }

    // Backstop for a lost AUDIO_STOP: close the session once audio has stopped
    // arriving, or (if configured) once it exceeds the length limit.
    if (s_state == BLE_STATE_TALKING) {
        // s_aud_last_ms is written by the NimBLE host task, so a notification
        // can land after `now` was sampled. The difference must be signed: as
        // unsigned it wrapped to ~4e9 and closed a live session mid-sentence.
        uint32_t last_audio = s_aud_last_ms ? s_aud_last_ms : s_talking_start_ms;
        bool idle = (int32_t)(now - last_audio) > VOICE_AUDIO_IDLE_MAX_MS;
        uint32_t max_ms = (uint32_t)key_engine_get_voice_config(&g_key_engine).max_sec * 1000u;
        bool too_long = max_ms > 0 && (int32_t)(now - s_talking_start_ms) > (int32_t)max_ms;
        if (idle || too_long) {
            app_log("ATVV", "Voice session %s -> closing microphone",
                    idle ? "went silent" : "hit length limit");
            voice_session_end(true);
        }
    }

    // Refresh the remote battery level periodically while connected.
    if ((s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_TALKING) &&
        s_batt_level_chr && (now - s_battery_last_ms) > BATTERY_REFRESH_INTERVAL_MS) {
        read_battery();
    }

    // Persist bound remote when connected.
    if ((s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_TALKING) && s_connected_mac[0] &&
        !mac_equals(s_bound_mac, s_connected_mac)) {
        strncpy(s_bound_mac, s_connected_mac, sizeof(s_bound_mac) - 1);
        strncpy(s_bound_name, s_connected_name, sizeof(s_bound_name) - 1);
        s_bound_type = s_pending_type;
        char typebuf[4];
        snprintf(typebuf, sizeof(typebuf), "%u", (unsigned)s_bound_type);
        config_store_set_str("ble_conf", "bound_mac", s_bound_mac);
        config_store_set_str("ble_conf", "bound_name", s_bound_name);
        config_store_set_str("ble_conf", "bound_type", typebuf);
    }
}

ble_remote_state_t ble_remote_get_state(void)
{
    return s_state;
}

void ble_remote_trigger_reconnect(void)
{
    s_req_reconnect = true;
}

bool ble_remote_connect_target(const char *mac_str, uint8_t addr_type, const char *dev_name)
{
    if (!mac_str || !mac_str[0]) return false;
    strncpy(s_pending_mac, mac_str, sizeof(s_pending_mac) - 1);
    s_pending_type = addr_type;
    strncpy(s_pending_name, (dev_name && dev_name[0]) ? dev_name : "Xiaomi Voice Remote",
            sizeof(s_pending_name) - 1);
    s_do_connect = true;
    app_log("BLE", "Manual connect queued: %s (%s)", s_pending_name, s_pending_mac);
    return true;
}

bool ble_remote_connect_mac(const char *mac_str)
{
    uint8_t type = 1;
    char name[40] = {0};
    portENTER_CRITICAL(&s_disc_mux);
    for (int i = 0; i < s_discovered_count; i++) {
        if (mac_equals(s_discovered[i].mac, mac_str)) {
            type = s_discovered[i].type;
            strncpy(name, s_discovered[i].name, sizeof(name) - 1);
            break;
        }
    }
    portEXIT_CRITICAL(&s_disc_mux);
    return ble_remote_connect_target(mac_str, type, name);
}

void ble_remote_unpair(void)
{
    s_bound_mac[0] = '\0';
    s_bound_name[0] = '\0';
    config_store_erase_key("ble_conf", "bound_mac");
    config_store_erase_key("ble_conf", "bound_name");
    config_store_erase_key("ble_conf", "bound_type");
    s_req_unpair = true;
    app_log("BLE", "Unpair requested");
}

size_t ble_remote_scan_devices_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    size_t w = 0;
    w += (size_t)snprintf(out + w, out_len - w, "{\"devices\":[");
    portENTER_CRITICAL(&s_disc_mux);
    for (int i = 0; i < s_discovered_count; i++) {
        w += (size_t)snprintf(out + w, out_len - w,
                              "%s{\"name\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"type\":%u}",
                              (i == 0) ? "" : ",",
                              s_discovered[i].name[0] ? s_discovered[i].name : "Unnamed",
                              s_discovered[i].mac, s_discovered[i].rssi, s_discovered[i].type);
        if (w >= out_len - 4) break;
    }
    portEXIT_CRITICAL(&s_disc_mux);
    w += (size_t)snprintf(out + w, out_len - w, "]}");
    return w;
}

size_t ble_remote_get_connected_info(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    return (size_t)snprintf(out, out_len,
                            "{\"connected\":%s,\"state\":%d,\"name\":\"%s\",\"mac\":\"%s\","
                            "\"bound_mac\":\"%s\",\"bound_name\":\"%s\",\"battery\":%d}",
                            (s_state >= BLE_STATE_CONNECTED) ? "true" : "false",
                            (int)s_state, s_connected_name, s_connected_mac,
                            s_bound_mac, s_bound_name, s_battery_level);
}

int ble_remote_get_battery(void)
{
    return s_battery_level;
}
