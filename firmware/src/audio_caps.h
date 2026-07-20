#ifndef PICOARC_AUDIO_CAPS_H
#define PICOARC_AUDIO_CAPS_H

#include <stdbool.h>
#include <stdint.h>

// Immutable core-0 -> core-1 snapshot of the formats learned through CEC SADs.
// Keeping this as a value type lets the realtime core gate audio without reading
// ARC state while core 0 is updating it.
typedef struct {
    bool arc_initiated;
    bool caps_ready;
    uint8_t pcm_rates_16;
    uint8_t pcm_rates_20;
    uint8_t pcm_rates_24;
    uint8_t ac3_rates;
    uint8_t dts_rates;
} picoarc_audio_caps_t;

#endif
