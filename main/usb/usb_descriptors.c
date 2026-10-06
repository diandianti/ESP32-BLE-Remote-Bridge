#include "usb_descriptors.h"
#include "app_config.h"
#include "version.h"

#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "esp_mac.h"

// ===========================================================================
// Device descriptor
// ===========================================================================
const tusb_desc_device_t usb_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, // >= 2.1 is required for BOS / WebUSB
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,      // Espressif
    .idProduct = 0x8304,     // MI-RC003 bridge
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// ===========================================================================
// String descriptors
// ===========================================================================
const char *usb_string_descriptors[] = {
    (char[]){0x09, 0x04},        // 0: language id (English US)
    "HD838A",                    // 1: manufacturer
    "MI-RC003 Remote Bridge",     // 2: product
    "MI-RC003-0001",              // 3: serial
    WEBUSB_LANDING_URL,          // 4: WebUSB landing page
    "MI-RC003 HID",               // 5: HID interface
    "MI-RC003 Microphone",        // 6: UAC microphone
    "MI-RC003 Interface",         // 7: WebUSB vendor interface (esp_tinyusb allows max 8)
};
const int usb_string_descriptor_count =
    (int)(sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0]));

// The serial number must be unique per chip. Windows keys the device instance
// on VID/PID/serial; a fixed serial makes Windows reuse a stale, incompatible
// registry entry ("device settings were not migrated", Code 10/28).
//
// The audio format is part of the serial for the same reason, and it is not
// cosmetic. Windows caches each capture endpoint's *default format* in the
// registry, keyed on this device instance, and opens the stream in shared mode
// at that cached rate. Change the sample rate in the descriptor without changing
// the serial and Windows keeps asking for the old rate: the device no longer
// supports it, the host's IAudioClient::Initialize fails, and the application
// gets NotReadableError - without ever selecting the streaming alternate
// setting, so the firmware sees nothing at all and reports zero USB reads.
//
// Encoding the rate here makes a format change look like new hardware, so the
// endpoint is rebuilt from the current descriptor instead of the stale cache.
// Bump nothing by hand: this follows AUDIO_SAMPLE_RATE automatically.
static char s_serial[24];

void usb_descriptors_init(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X-%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], AUDIO_SAMPLE_RATE);
    usb_string_descriptors[3] = s_serial;
}

// ===========================================================================
// HID report descriptor: keyboard (report ID 1) + consumer (report ID 2)
// + relative mouse (report ID 3: 5 buttons, X/Y, wheel)
// ===========================================================================
const uint8_t usb_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(2)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(3)),
};
const uint16_t usb_hid_report_descriptor_len = sizeof(usb_hid_report_descriptor);

// ===========================================================================
// Configuration descriptor
//
//   ITF 0/1 : UAC 1.0 microphone (custom class driver, 108-byte descriptor set)
//   ITF 2   : HID keyboard + consumer control
//   ITF 3   : WebUSB vendor-specific bulk interface
// ===========================================================================
#define USB_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 108 + TUD_HID_DESC_LEN + \
                               TUD_VENDOR_DESC_LEN)

const uint8_t usb_config_descriptor[] = {
    // Configuration number, interface count, string index, total length, attributes, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, USB_CONFIG_TOTAL_LEN,
                          0, 100),

    // -------------------- UAC 1.0 microphone (108 bytes) --------------------
    // 1. IAD (bFirstInterface=0, bInterfaceCount=2, audio, iFunction=6)
    0x08, 0x0B, USB_ITF_UAC_AC, 0x02, 0x01, 0x00, 0x00, 6,
    // 2. Standard AC interface
    0x09, 0x04, USB_ITF_UAC_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 6,
    // 3. CS AC header
    0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, USB_ITF_UAC_AS,
    // 4. Input terminal (microphone)
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    // 5. Feature unit (mute + volume)
    0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00,
    // 6. Output terminal (USB streaming)
    0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x00,
    // 7. Standard AS interface, alt 0 (zero bandwidth)
    0x09, 0x04, USB_ITF_UAC_AS, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    // 8. Standard AS interface, alt 1 (active)
    0x09, 0x04, USB_ITF_UAC_AS, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    // 9. CS AS general
    0x07, 0x24, 0x01, 0x03, 0x01, 0x01, 0x00,
    // 10. Format type I: AUDIO_SAMPLE_RATE, 16-bit, mono. The three sample-rate
    //     bytes are little endian, derived from the active sample rate so the
    //     descriptor can never disagree with what the DSP actually produces.
    0x0B, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x01,
    (uint8_t)(AUDIO_SAMPLE_RATE & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 8) & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 16) & 0xFF),
    // 11. Isochronous IN endpoint, bInterval = 2 ms.
    //     The ESP32-S3 DWC2 even/odd frame-boundary bug means queuing
    //     isochronous IN transfers on a 1 ms cadence makes the controller skip
    //     every other frame, halving the effective rate; 2 ms leaves a whole
    //     frame of slack. USB_UAC_PACKET_BYTES is exactly 2 ms of mono 16-bit
    //     audio at AUDIO_SAMPLE_RATE, which is what the host's bandwidth check
    //     requires (64 B / 2 ms = 32 kB/s at 16 kHz, 32 B / 2 ms = 16 kB/s at
    //     8 kHz).
    0x09, 0x05, USB_EP_UAC_IN, 0x05,
    (uint8_t)(USB_UAC_PACKET_BYTES & 0xFF),
    (uint8_t)((USB_UAC_PACKET_BYTES >> 8) & 0xFF),
    0x02, 0x00, 0x00,
    // 12. CS endpoint general
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // -------------------- HID keyboard + consumer --------------------
    // Interface number, string index, boot protocol, report descriptor len,
    // EP IN address, EP size, polling interval (ms)
    TUD_HID_DESCRIPTOR(USB_ITF_HID, 5, false, sizeof(usb_hid_report_descriptor),
                       USB_EP_HID_IN, 16, 5),

    // -------------------- WebUSB vendor interface --------------------
    // Interface number, string index, EP OUT address, EP IN address, EP size
    TUD_VENDOR_DESCRIPTOR(USB_ITF_VENDOR, 7, USB_EP_VENDOR_OUT, USB_EP_VENDOR_IN, 64),
};

TU_VERIFY_STATIC(sizeof(usb_config_descriptor) == USB_CONFIG_TOTAL_LEN,
                 "configuration descriptor total length mismatch");

// ===========================================================================
// Binary Object Store (WebUSB + Microsoft OS 2.0)
//
// Windows does not auto-bind a driver for a generic vendor interface. The
// Microsoft OS 2.0 descriptor set advertises the "WINUSB" compatible ID so
// Windows loads winusb.sys and Chrome can reach the interface via WebUSB.
// ===========================================================================
#define VENDOR_REQUEST_MICROSOFT  0x02
#define MS_OS_20_DESC_LEN         0xB2

#define USB_BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t usb_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(USB_BOS_TOTAL_LEN, 2),
    TUD_BOS_WEBUSB_DESCRIPTOR(WEBUSB_VENDOR_CODE, 4),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return usb_bos_descriptor;
}

// Microsoft OS 2.0 descriptor set: WINUSB compatible ID + DeviceInterfaceGUIDs.
static const uint8_t desc_ms_os_20[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    USB_ITF_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // Compatible ID descriptor: length, type, "WINUSB", sub-compatible
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property descriptor: DeviceInterfaceGUIDs
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    '{', 0x00, '9', 0x00, '7', 0x00, '5', 0x00, 'F', 0x00, '4', 0x00, '4', 0x00, 'D', 0x00,
    '9', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00,
    '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00, '8', 0x00, 'A', 0x00,
    'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size mismatch");

// ===========================================================================
// WebUSB landing page URL descriptor
// ===========================================================================
static const tusb_desc_webusb_url_t usb_url_descriptor = {
    .bLength = (uint8_t)(3 + sizeof(WEBUSB_LANDING_URL) - 1),
    .bDescriptorType = 3, // WEBUSB URL descriptor type
    .bScheme = WEBUSB_LANDING_SCHEME,
    .url = WEBUSB_LANDING_URL,
};

// Invoked for every vendor-type control request. WebUSB fetches the landing
// page URL, and Windows fetches the Microsoft OS 2.0 descriptor set.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        if (request->bRequest == WEBUSB_VENDOR_CODE) {
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)&usb_url_descriptor,
                                    usb_url_descriptor.bLength);
        }
        if (request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7) {
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20 + 8, 2);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, total_len);
        }
    }
    return false;
}

// ===========================================================================
// HID callbacks
// ===========================================================================
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return usb_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
