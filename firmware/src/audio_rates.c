#include "usb_descriptors.h"

uint32_t usb_audio_rate_at(unsigned int index) {
    static const uint32_t rates[] = {
        PICOARC_AUDIO_SAMPLE_RATE_32K, PICOARC_AUDIO_SAMPLE_RATE_44K1,
        PICOARC_AUDIO_SAMPLE_RATE_48K, PICOARC_AUDIO_SAMPLE_RATE_88K2,
        PICOARC_AUDIO_SAMPLE_RATE_96K,
    };
    return index < PICOARC_AUDIO_SAMPLE_RATE_COUNT ? rates[index] : 0;
}

bool usb_audio_rate_in_mask(uint8_t mask, uint32_t rate) {
    for (unsigned int i = 0; i < PICOARC_AUDIO_SAMPLE_RATE_COUNT; ++i) {
        if (rate == usb_audio_rate_at(i)) {
            return (mask & (1u << i)) != 0;
        }
    }
    return false;
}

uint32_t usb_audio_nearest_rate(uint8_t mask, uint32_t requested) {
    uint32_t closest = 0;
    uint32_t distance = UINT32_MAX;
    for (unsigned int i = 0; i < PICOARC_AUDIO_SAMPLE_RATE_COUNT; ++i) {
        if (!(mask & (1u << i))) {
            continue;
        }
        const uint32_t rate = usb_audio_rate_at(i);
        const uint32_t delta = requested > rate ? requested - rate : rate - requested;
        if (delta < distance) {
            closest = rate;
            distance = delta;
        }
    }
    return closest;
}
