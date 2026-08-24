#ifndef PICOARC_USB_DESCRIPTORS_H
#define PICOARC_USB_DESCRIPTORS_H

#ifndef PICOARC_DEBUG_USB
#define PICOARC_DEBUG_USB 1
#endif

#ifndef PICOARC_UAC_VERSION
#define PICOARC_UAC_VERSION 1
#endif

#if PICOARC_UAC_VERSION != 1 && PICOARC_UAC_VERSION != 2
#error PICOARC_UAC_VERSION must be 1 or 2
#endif

#define PICOARC_AUDIO_EP_OUT            0x03
#define PICOARC_AUDIO_EP_FB             0x83
#define PICOARC_AUDIO_EP_INT            0x84

#if PICOARC_DEBUG_USB
#define ITF_NUM_CDC                     0
#define ITF_NUM_CDC_DATA                1
#define ITF_NUM_AUDIO_CONTROL           2
#define ITF_NUM_AUDIO_STREAMING         3
#define ITF_NUM_RESET                   4
#define ITF_NUM_TOTAL                   5
#else
#define ITF_NUM_AUDIO_CONTROL           0
#define ITF_NUM_AUDIO_STREAMING         1
#define ITF_NUM_TOTAL                   2
#endif

// Streaming alternates shared by UAC1 and UAC2. UAC2 adds IEC 61937 alternates
// in its class-specific descriptor header.
#define PICOARC_AUDIO_ALT_ZERO          0
#define PICOARC_AUDIO_ALT_PCM_16        1
#define PICOARC_AUDIO_ALT_PCM_20        2
#define PICOARC_AUDIO_ALT_PCM_24        3

#define PICOARC_AUDIO_SAMPLE_RATE_32K   32000
#define PICOARC_AUDIO_SAMPLE_RATE_44K1  44100
#define PICOARC_AUDIO_SAMPLE_RATE_48K   48000
#define PICOARC_AUDIO_SAMPLE_RATE_88K2  88200
#define PICOARC_AUDIO_SAMPLE_RATE_96K   96000
#define PICOARC_AUDIO_SAMPLE_RATE_COUNT 5

#define PICOARC_AUDIO_SAMPLE_RATE_DEFAULT PICOARC_AUDIO_SAMPLE_RATE_48K
#define PICOARC_AUDIO_SAMPLE_RATE_MAX     PICOARC_AUDIO_SAMPLE_RATE_96K

#define PICOARC_AUDIO_RATE_BIT_32K   (1u << 0)
#define PICOARC_AUDIO_RATE_BIT_44K1  (1u << 1)
#define PICOARC_AUDIO_RATE_BIT_48K   (1u << 2)
#define PICOARC_AUDIO_RATE_BIT_88K2  (1u << 3)
#define PICOARC_AUDIO_RATE_BIT_96K   (1u << 4)
#define PICOARC_AUDIO_USB_RATE_MASK \
    (PICOARC_AUDIO_RATE_BIT_32K | PICOARC_AUDIO_RATE_BIT_44K1 | \
     PICOARC_AUDIO_RATE_BIT_48K | PICOARC_AUDIO_RATE_BIT_88K2 | \
     PICOARC_AUDIO_RATE_BIT_96K)

#define PICOARC_AUDIO_CHANNELS          2
// EP wMaxPacketSize is sized for the highest sample rate the device advertises.
// Full-speed USB cannot carry 2ch 20/24-bit PCM above 96 kHz.
#define PICOARC_AUDIO_EP_OUT_SZ_16      TUD_AUDIO_EP_SIZE(false, PICOARC_AUDIO_SAMPLE_RATE_MAX, 2, PICOARC_AUDIO_CHANNELS)
#define PICOARC_AUDIO_EP_OUT_SZ_3B      TUD_AUDIO_EP_SIZE(false, PICOARC_AUDIO_SAMPLE_RATE_MAX, 3, PICOARC_AUDIO_CHANNELS)

#if PICOARC_UAC_VERSION == 1
#include "usb_descriptors_uac1.h"
#else
#include "usb_descriptors_uac2.h"
#endif

void usb_descriptors_set_audio_name(const char *name);
void usb_descriptors_reset_audio_name(void);

#endif
