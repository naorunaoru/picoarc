#ifndef PICOARC_AUDIO_SAMPLES_H
#define PICOARC_AUDIO_SAMPLES_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decode 2- or 3-byte little-endian USB audio subslots into the
 * 24-bit-left-aligned representation consumed by the S/PDIF encoder.
 * Incomplete trailing input is ignored and the number of complete samples
 * written is bounded by output_capacity. Input and output must address arrays
 * large enough for the supplied sizes; both may be NULL when their size or
 * capacity is zero. Unsupported subslot widths return zero.
 */
size_t audio_samples_decode_le(int32_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_size,
                               unsigned int bytes_per_sample);

#endif
