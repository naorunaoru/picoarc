#include <string.h>

#include "pico/unique_id.h"
#if PICOARC_DEBUG_USB
#include "pico/stdio_usb/reset_interface.h"
#endif
#include "tusb.h"
#include "usb_descriptors.h"
#include "arc.h"

#define USB_VID 0xcafe
// Every audio-class/layout combination gets its own PID so host descriptor and
// class-driver caches cannot cross-bind UAC1, UAC2, release, or debug builds.
#if PICOARC_UAC_VERSION == 1
#if PICOARC_DEBUG_USB
#define USB_PID 0x4013
#else
#define USB_PID 0x4012
#endif
#else
#if PICOARC_DEBUG_USB
#define USB_PID 0x4017
#else
#define USB_PID 0x4016
#endif
#endif
#define USB_BCD 0x0202
#define USB_STRING_MAX_CHARS 32

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

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
#if PICOARC_DEBUG_USB || PICOARC_UAC_VERSION == 2
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
#endif
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

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + DEBUG_USB_DESC_LEN \
    + PICOARC_AUDIO_IAD_DESC_LEN + TUD_AUDIO_PICOARC_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 250),
#if PICOARC_DEBUG_USB
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#if PICOARC_UAC_VERSION == 1
    TUD_AUDIO10_PICOARC_DESC_IAD(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO),
#endif
#endif
    TUD_AUDIO_PICOARC_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO,
                                 PICOARC_AUDIO_EP_OUT,
                                 PICOARC_AUDIO_EP_FB,
                                 PICOARC_AUDIO_EP_INT),
#if PICOARC_DEBUG_USB
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STRID_RESET),
#endif
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "USB configuration descriptor length mismatch");

static uint8_t audio_configuration[CONFIG_TOTAL_LEN];
static uint8_t audio_rates[PICOARC_AUDIO_ALT_MAX + 1];
static uint8_t audio_formats[PICOARC_AUDIO_ALT_MAX + 1];

uint8_t usb_descriptors_audio_format(uint8_t usb_alt) {
    return usb_alt <= PICOARC_AUDIO_ALT_MAX ? audio_formats[usb_alt] : 0;
}

uint8_t usb_descriptors_audio_rate_mask(uint8_t format) {
    if (format > PICOARC_AUDIO_ALT_MAX) {
        return 0;
    }
    if (format != 0) {
        return audio_rates[format];
    }
    // UAC2 has one physical clock shared by all streaming formats.
    uint8_t rates = 0;
    for (unsigned int i = 1; i <= PICOARC_AUDIO_ALT_MAX; ++i) {
        rates |= audio_rates[i];
    }
    return rates;
}

bool usb_descriptors_audio_changed(void) {
    for (unsigned int i = 1; i <= PICOARC_AUDIO_ALT_MAX; ++i) {
        if (audio_rates[i] != arc_audio_rate_mask(i)) {
            return true;
        }
    }
    return false;
}

void usb_descriptors_prepare_audio(void) {
    memset(audio_formats, 0, sizeof(audio_formats));
    for (unsigned int i = 1; i <= PICOARC_AUDIO_ALT_MAX; ++i) {
        audio_rates[i] = arc_audio_rate_mask(i);
    }
    unsigned int used = 0;
    uint8_t next_alt = 0;
    uint8_t format = 0;
    bool skip = false;
    bool streaming_interface = false;
    for (unsigned int offset = 0; offset < sizeof(desc_configuration);) {
        const uint8_t *source = desc_configuration + offset;
        const unsigned int length = source[0];
        if (source[1] == TUSB_DESC_INTERFACE) {
            streaming_interface = source[2] == ITF_NUM_AUDIO_STREAMING;
            format = streaming_interface ? source[3] : 0;
            skip = format != 0 && audio_rates[format] == 0;
        }
        if (!skip) {
            uint8_t *dest = audio_configuration + used;
            memcpy(dest, source, length);
            unsigned int out_length = length;
            if (streaming_interface && source[1] == TUSB_DESC_INTERFACE && format != 0) {
                dest[3] = ++next_alt;
                audio_formats[next_alt] = format;
            }
#if PICOARC_UAC_VERSION == 1
            if (streaming_interface && format != 0 && source[1] == TUSB_DESC_CS_INTERFACE &&
                source[2] == AUDIO10_CS_AS_INTERFACE_FORMAT_TYPE) {
                unsigned int count = 0;
                for (unsigned int i = 0; i < PICOARC_AUDIO_SAMPLE_RATE_COUNT; ++i) {
                    const uint32_t rate = usb_audio_rate_at(i);
                    if (usb_audio_rate_in_mask(audio_rates[format], rate)) {
                        dest[8 + 3 * count] = (uint8_t)rate;
                        dest[9 + 3 * count] = (uint8_t)(rate >> 8);
                        dest[10 + 3 * count] = (uint8_t)(rate >> 16);
                        ++count;
                    }
                }
                out_length = 8 + 3 * count;
                dest[0] = (uint8_t)out_length;
                dest[7] = (uint8_t)count;
            }
#endif
            used += out_length;
        }
        offset += length;
    }
    audio_configuration[2] = (uint8_t)used;
    audio_configuration[3] = (uint8_t)(used >> 8);
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return audio_configuration;
}

static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
static char product_str[USB_STRING_MAX_CHARS + 1] = "PicoARC USB Audio";
static char audio_str[USB_STRING_MAX_CHARS + 1] = "PicoARC Speaker";

static char const *const string_desc_arr[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "PicoARC",
    [STRID_PRODUCT] = product_str,
    [STRID_SERIAL] = serial_str,
#if PICOARC_DEBUG_USB
    [STRID_CDC] = "PicoARC Debug",
#endif
    [STRID_AUDIO] = audio_str,
#if PICOARC_DEBUG_USB
    [STRID_RESET] = "PicoARC Reset",
#endif
};

void usb_descriptors_reset_audio_name(void) {
    strcpy(product_str, "PicoARC USB Audio");
    strcpy(audio_str, "PicoARC Speaker");
}

void usb_descriptors_set_audio_name(const char *name) {
#if PICOARC_DEBUG_USB
    (void)name;
#else
    if (name == NULL || name[0] == '\0') {
        usb_descriptors_reset_audio_name();
        return;
    }

    size_t len = strlen(name);
    if (len > USB_STRING_MAX_CHARS) {
        len = USB_STRING_MAX_CHARS;
    }

    if (len == 0) {
        usb_descriptors_reset_audio_name();
        return;
    }

    memcpy(product_str, name, len);
    product_str[len] = '\0';
    memcpy(audio_str, name, len);
    audio_str[len] = '\0';
#endif
}

static uint16_t desc_str[USB_STRING_MAX_CHARS + 1];

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
        if (chr_count > USB_STRING_MAX_CHARS) {
            chr_count = USB_STRING_MAX_CHARS;
        }

        for (size_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
