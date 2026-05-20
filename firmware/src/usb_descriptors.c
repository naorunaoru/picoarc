#include <string.h>

#include "pico/unique_id.h"
#if PICOARC_DEBUG_USB
#include "pico/stdio_usb/reset_interface.h"
#endif
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0xcafe
#define USB_PID 0x4011
#define USB_BCD 0x0100

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_AUDIO_OUT 0x03
#define EPNUM_AUDIO_FB  0x83
#define EPNUM_AUDIO_INT 0x84

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
#if PICOARC_DEBUG_USB
    STRID_CDC,
#endif
    STRID_AUDIO,
#if PICOARC_DEBUG_USB
    STRID_RESET,
#endif
};

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

#define TUD_RPI_RESET_DESC_LEN 9
#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, _stridx

#if PICOARC_DEBUG_USB
#define DEBUG_USB_DESC_LEN (TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)
#else
#define DEBUG_USB_DESC_LEN 0
#endif

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + DEBUG_USB_DESC_LEN + TUD_AUDIO_PICOARC_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 250),
#if PICOARC_DEBUG_USB
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
    TUD_AUDIO_PICOARC_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO,
                                 EPNUM_AUDIO_OUT,
                                 EPNUM_AUDIO_FB,
                                 3,
                                 EPNUM_AUDIO_INT),
#if PICOARC_DEBUG_USB
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STRID_RESET),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static char const *const string_desc_arr[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "PicoARC",
    [STRID_PRODUCT] = "PicoARC USB Audio",
    [STRID_SERIAL] = serial_str,
#if PICOARC_DEBUG_USB
    [STRID_CDC] = "PicoARC Debug",
#endif
    [STRID_AUDIO] = "PicoARC Speaker",
#if PICOARC_DEBUG_USB
    [STRID_RESET] = "PicoARC Reset",
#endif
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (!serial_str[0]) {
        pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
    }

    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc_arr) || string_desc_arr[index] == NULL) {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 32) {
            chr_count = 32;
        }

        for (size_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
