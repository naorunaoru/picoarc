#ifndef PICOARC_PCM_CONVERT_H
#define PICOARC_PCM_CONVERT_H
#include <stdint.h>
static inline int32_t pcm16_to_internal(int16_t sample) {
    return (int32_t)sample * INT32_C(65536);
}
#endif
