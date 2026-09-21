#include "audio_samples.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    MAX_BATCH_FRAMES = 192,
    CHANNELS = 2,
    MAX_BATCH_SAMPLES = MAX_BATCH_FRAMES * CHANNELS,
};

static unsigned int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #condition);                                                 \
            failures++;                                                         \
        }                                                                        \
    } while (0)

static void check_16_bit_boundaries_and_iec_preambles(void) {
    static const uint8_t input[] = {
        0x00, 0x80, /* -32768 */
        0xff, 0xff, /* -1 */
        0x00, 0x00, /* 0 */
        0x01, 0x00, /* 1 */
        0xff, 0x7f, /* 32767 */
        0x72, 0xf8, /* IEC 61937 Pa */
        0x1f, 0x4e, /* IEC 61937 Pb */
    };
    static const uint32_t expected[] = {
        UINT32_C(0x80000000), UINT32_C(0xffff0000), UINT32_C(0x00000000),
        UINT32_C(0x00010000), UINT32_C(0x7fff0000), UINT32_C(0xf8720000),
        UINT32_C(0x4e1f0000),
    };
    int32_t output[sizeof(expected) / sizeof(expected[0])] = {0};

    const size_t count = audio_samples_decode_le(
        output, sizeof(output) / sizeof(output[0]), input, sizeof(input), 2);
    CHECK(count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < count; i++) {
        CHECK((uint32_t)output[i] == expected[i]);
    }
}

static void check_20_and_24_bit_boundaries(void) {
    static const uint8_t input[] = {
        0x00, 0x00, 0x80, /* 24-bit and left-justified 20-bit minimum */
        0xff, 0xff, 0xff, /* 24-bit -1 */
        0xf0, 0xff, 0xff, /* left-justified 20-bit -1 */
        0x00, 0x00, 0x00, /* zero */
        0x01, 0x00, 0x00, /* 24-bit 1 */
        0xf0, 0xff, 0x7f, /* left-justified 20-bit maximum */
        0xff, 0xff, 0x7f, /* 24-bit maximum */
    };
    static const uint32_t expected[] = {
        UINT32_C(0x80000000), UINT32_C(0xffffff00), UINT32_C(0xfffff000),
        UINT32_C(0x00000000), UINT32_C(0x00000100), UINT32_C(0x7ffff000),
        UINT32_C(0x7fffff00),
    };
    int32_t output[sizeof(expected) / sizeof(expected[0])] = {0};

    const size_t count = audio_samples_decode_le(
        output, sizeof(output) / sizeof(output[0]), input, sizeof(input), 3);
    CHECK(count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < count; i++) {
        CHECK((uint32_t)output[i] == expected[i]);
    }
}

static void check_bounds_truncation_and_unaligned_input(void) {
    _Alignas(4) static const uint8_t storage[] = {
        0xaa, 0x34, 0x12, 0xfe, 0xff, 0xbb,
    };
    int32_t output[] = {INT32_C(0x11111111), INT32_C(0x22222222)};

    CHECK(audio_samples_decode_le(output, 2, storage + 1, 4, 2) == 2);
    CHECK((uint32_t)output[0] == UINT32_C(0x12340000));
    CHECK((uint32_t)output[1] == UINT32_C(0xfffe0000));

    output[0] = INT32_C(0x11111111);
    output[1] = INT32_C(0x22222222);
    CHECK(audio_samples_decode_le(output, 1, storage + 1, 4, 2) == 1);
    CHECK(output[0] == INT32_C(0x12340000));
    CHECK(output[1] == INT32_C(0x22222222));

    output[0] = INT32_C(0x11111111);
    CHECK(audio_samples_decode_le(output, 2, storage + 1, 3, 2) == 1);
    CHECK(output[0] == INT32_C(0x12340000));

    CHECK(audio_samples_decode_le(output, 2, storage + 1, 1, 2) == 0);
    CHECK(audio_samples_decode_le(output, 2, storage + 1, 4, 0) == 0);
    CHECK(audio_samples_decode_le(output, 0, storage + 1, 4, 2) == 0);
    CHECK(audio_samples_decode_le(NULL, 0, NULL, 0, 2) == 0);
}

static void check_24_bit_exact_allocations_and_truncation(void) {
    uint8_t *input = malloc(5);
    int32_t *output = malloc(2 * sizeof(*output));
    CHECK(input != NULL);
    CHECK(output != NULL);
    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        return;
    }

    input[0] = 0x56;
    input[1] = 0x34;
    input[2] = 0x12;
    input[3] = 0xaa;
    input[4] = 0xbb;
    output[0] = INT32_C(0x11111111);
    output[1] = INT32_C(0x22222222);
    CHECK(audio_samples_decode_le(output, 2, input, 5, 3) == 1);
    CHECK((uint32_t)output[0] == UINT32_C(0x12345600));
    CHECK(output[1] == INT32_C(0x22222222));

    free(output);
    free(input);

    input = malloc(6);
    output = malloc(2 * sizeof(*output));
    CHECK(input != NULL);
    CHECK(output != NULL);
    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        return;
    }
    for (size_t i = 0; i < 6; i++) {
        input[i] = (uint8_t)(i + 1u);
    }
    output[1] = INT32_C(0x33333333);
    CHECK(audio_samples_decode_le(output, 1, input, 6, 3) == 1);
    CHECK((uint32_t)output[0] == UINT32_C(0x03020100));
    CHECK(output[1] == INT32_C(0x33333333));

    free(output);
    free(input);
}

static uint32_t expected_s24(const uint8_t *input) {
    const uint32_t raw = (uint32_t)input[0] |
                         ((uint32_t)input[1] << 8) |
                         ((uint32_t)input[2] << 16);
    return raw << 8;
}

static void check_maximum_firmware_batch(void) {
    const size_t input_size = MAX_BATCH_SAMPLES * 3u;
    uint8_t *input = malloc(input_size);
    int32_t *output = malloc(MAX_BATCH_SAMPLES * sizeof(*output));
    CHECK(input != NULL);
    CHECK(output != NULL);
    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        return;
    }

    for (size_t i = 0; i < input_size; i++) {
        input[i] = (uint8_t)(i * 73u + 19u);
    }
    CHECK(audio_samples_decode_le(output, MAX_BATCH_SAMPLES,
                                  input, input_size, 3) == MAX_BATCH_SAMPLES);
    for (size_t i = 0; i < MAX_BATCH_SAMPLES; i++) {
        CHECK((uint32_t)output[i] == expected_s24(input + i * 3u));
    }

    free(output);
    free(input);
}

int main(void) {
    check_16_bit_boundaries_and_iec_preambles();
    check_20_and_24_bit_boundaries();
    check_bounds_truncation_and_unaligned_input();
    check_24_bit_exact_allocations_and_truncation();
    check_maximum_firmware_batch();

    if (failures != 0) {
        fprintf(stderr, "%u check(s) failed\n", failures);
        return 1;
    }
    puts("audio_samples_test: all checks passed");
    return 0;
}
