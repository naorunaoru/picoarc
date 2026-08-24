#ifndef PICOARC_USB_DESCRIPTORS_UAC1_H
#define PICOARC_USB_DESCRIPTORS_UAC1_H

#define PICOARC_AUDIO_ENTITY_INPUT_TERMINAL   0x01
#define PICOARC_AUDIO_ENTITY_FEATURE_UNIT     0x02
#define PICOARC_AUDIO_ENTITY_OUTPUT_TERMINAL  0x03

#define PICOARC_AUDIO_ALT_MAX PICOARC_AUDIO_ALT_PCM_24
#define PICOARC_AUDIO_HAS_FEEDBACK_EP 1
#define PICOARC_AUDIO_HAS_INTERRUPT_EP 0

// The debug configuration already contains the CDC function's IAD. Windows
// therefore requires the UAC1 interfaces to be grouped by an IAD as well. A
// standalone UAC1 release function uses the canonical AC header collection.
#if PICOARC_DEBUG_USB
#define PICOARC_AUDIO_IAD_DESC_LEN 8
#define TUD_AUDIO10_PICOARC_DESC_IAD(_firstitf, _stridx) \
    PICOARC_AUDIO_IAD_DESC_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, _firstitf, 0x02, \
    TUSB_CLASS_AUDIO, 0x00, 0x00, _stridx
#else
#define PICOARC_AUDIO_IAD_DESC_LEN 0
#endif

#define PICOARC_AUDIO_CS_AC_DESC_LEN (TUD_AUDIO10_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(PICOARC_AUDIO_CHANNELS) \
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define PICOARC_AUDIO_STREAMING_ALT_DESC_LEN (TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(PICOARC_AUDIO_SAMPLE_RATE_COUNT) \
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP_LEN)

#define TUD_AUDIO_PICOARC_DESC_LEN (TUD_AUDIO10_DESC_STD_AC_LEN \
    + TUD_AUDIO10_DESC_CS_AC_LEN(1) \
    + PICOARC_AUDIO_CS_AC_DESC_LEN \
    + TUD_AUDIO10_DESC_STD_AS_LEN /* alt 0 */ \
    + 3 * PICOARC_AUDIO_STREAMING_ALT_DESC_LEN)

#define TUD_AUDIO_PICOARC_STREAMING_ALT(_itfnum, _alt, _subframesize, _bitresolution, _epout, _epoutsize, _epfb) \
    TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), _alt, 0x02, 0x00), \
    TUD_AUDIO10_DESC_CS_AS_INT(PICOARC_AUDIO_ENTITY_INPUT_TERMINAL, 0x00, AUDIO10_DATA_FORMAT_TYPE_I_PCM), \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(PICOARC_AUDIO_CHANNELS, _subframesize, _bitresolution, \
                                   PICOARC_AUDIO_SAMPLE_RATE_32K, \
                                   PICOARC_AUDIO_SAMPLE_RATE_44K1, \
                                   PICOARC_AUDIO_SAMPLE_RATE_48K, \
                                   PICOARC_AUDIO_SAMPLE_RATE_88K2, \
                                   PICOARC_AUDIO_SAMPLE_RATE_96K), \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(_epout, \
                                   (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS), \
                                   _epoutsize, 0x01, _epfb), \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, \
                                  AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000), \
    /* UAC1 4.6.2.1: bRefresh=1 requests feedback every 2 ms. */ \
    TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(_epfb, 0x01)

#define TUD_AUDIO_PICOARC_DESCRIPTOR(_itfnum, _stridx, _epout, _epfb, _epint) \
    TUD_AUDIO10_DESC_STD_AC(_itfnum, 0x00, _stridx), \
    TUD_AUDIO10_DESC_CS_AC(0x0100, PICOARC_AUDIO_CS_AC_DESC_LEN, (uint8_t)((_itfnum) + 1)), \
    TUD_AUDIO10_DESC_INPUT_TERM(PICOARC_AUDIO_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, \
                                0x00, PICOARC_AUDIO_CHANNELS, \
                                AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, \
                                0x00, 0x00), \
    TUD_AUDIO10_DESC_FEATURE_UNIT(PICOARC_AUDIO_ENTITY_FEATURE_UNIT, \
                                  PICOARC_AUDIO_ENTITY_INPUT_TERMINAL, 0x00, \
                                  AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME, \
                                  0x0000, 0x0000), \
    TUD_AUDIO10_DESC_OUTPUT_TERM(PICOARC_AUDIO_ENTITY_OUTPUT_TERMINAL, \
                                 AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER, 0x00, \
                                 PICOARC_AUDIO_ENTITY_FEATURE_UNIT, 0x00), \
    /* AS alt 0: zero-bandwidth (mandatory). */ \
    TUD_AUDIO10_DESC_STD_AS_INT((uint8_t)((_itfnum) + 1), PICOARC_AUDIO_ALT_ZERO, 0x00, 0x00), \
    TUD_AUDIO_PICOARC_STREAMING_ALT(_itfnum, PICOARC_AUDIO_ALT_PCM_16, 2, 16, \
                                    _epout, PICOARC_AUDIO_EP_OUT_SZ_16, _epfb), \
    TUD_AUDIO_PICOARC_STREAMING_ALT(_itfnum, PICOARC_AUDIO_ALT_PCM_20, 3, 20, \
                                    _epout, PICOARC_AUDIO_EP_OUT_SZ_3B, _epfb), \
    TUD_AUDIO_PICOARC_STREAMING_ALT(_itfnum, PICOARC_AUDIO_ALT_PCM_24, 3, 24, \
                                    _epout, PICOARC_AUDIO_EP_OUT_SZ_3B, _epfb)

#endif
