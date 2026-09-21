#include "audio_samples.h"

#include <limits.h>

static int32_t decode_s16_le(const uint8_t *input) {
    const uint32_t raw = (uint32_t)input[0] | ((uint32_t)input[1] << 8);
    const int32_t sample = raw <= INT16_MAX
                               ? (int32_t)raw
                               : (int32_t)raw - (INT16_MAX + 1) * 2;
    return sample * INT32_C(65536);
}

static int32_t decode_s24_le(const uint8_t *input) {
    const uint32_t raw = (uint32_t)input[0] |
                         ((uint32_t)input[1] << 8) |
                         ((uint32_t)input[2] << 16);
    const int32_t sample = raw <= INT32_C(0x7fffff)
                               ? (int32_t)raw
                               : (int32_t)raw - INT32_C(0x1000000);
    return sample * INT32_C(256);
}

size_t audio_samples_decode_le(int32_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_size,
                               unsigned int bytes_per_sample) {
    if (bytes_per_sample != 2u && bytes_per_sample != 3u) {
        return 0;
    }

    size_t sample_count = input_size / bytes_per_sample;
    if (sample_count > output_capacity) {
        sample_count = output_capacity;
    }

    for (size_t i = 0; i < sample_count; i++) {
        const uint8_t *sample = input + i * bytes_per_sample;
        output[i] = bytes_per_sample == 2u
                        ? decode_s16_le(sample)
                        : decode_s24_le(sample);
    }
    return sample_count;
}
