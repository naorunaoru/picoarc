#include "spdif.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "spdif.pio.h"

enum {
    SPDIF_PIN_RATE_HZ = 6144000,
    SPDIF_FRAMES_PER_BLOCK = 192,
    SPDIF_WORDS_PER_SUBFRAME = 2,
    SPDIF_WORDS_PER_FRAME = 4,
    SPDIF_WORDS_PER_BLOCK = SPDIF_FRAMES_PER_BLOCK * SPDIF_WORDS_PER_FRAME,
    // One slot stays empty so read == write can unambiguously mean empty.
    USB_RING_FRAMES = 8192,
};

typedef enum {
    PREAMBLE_B,
    PREAMBLE_M,
    PREAMBLE_W,
} spdif_preamble_t;

typedef struct {
    uint16_t bits;
    uint8_t level;
    uint8_t parity;
} bmc_byte_t;

static const int16_t sine_1khz_48k[48] = {
    0, 1069, 2120, 3135, 4096, 4987, 5793, 6499,
    7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
    7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069,
    0, -1069, -2120, -3135, -4096, -4987, -5793, -6499,
    -7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568,
    -7094, -6499, -5793, -4987, -4096, -3135, -2120, -1069,
};

static uint32_t tone_words[SPDIF_WORDS_PER_BLOCK];
static uint32_t silence_words[SPDIF_WORDS_PER_BLOCK];
static uint32_t *build_words;
static size_t spdif_word_index;
static unsigned int current_level;
static PIO spdif_pio;
static unsigned int spdif_sm;
static unsigned int spdif_dma_chan;
static dma_channel_config spdif_dma_config;
static volatile spdif_mode_t current_mode = SPDIF_MODE_TONE_1KHZ;
// Samples are stored 24-bit-left-aligned in int32_t (audio MSB at bit 31,
// audio LSB at bit 8). 16-bit USB input is shifted left 16 on ingest, 24-bit
// USB input is unpacked from its 3-byte little-endian form. Either way the
// S/PDIF encoder sees the same internal representation.
static int32_t usb_ring[USB_RING_FRAMES][2];
static volatile unsigned int usb_ring_read;
static volatile unsigned int usb_ring_write;
static volatile unsigned int usb_underrun_frames;
static volatile unsigned int usb_high_water_frames;
static bmc_byte_t bmc_byte[2][256];
static uint8_t bmc_tail[2][2];
static uint32_t dma_words[2][SPDIF_WORDS_PER_BLOCK];

static unsigned int buffered_frames_from(unsigned int read, unsigned int write) {
    if (write >= read) {
        return write - read;
    }

    return USB_RING_FRAMES - read + write;
}

static void build_bmc_tables(void) {
    for (unsigned int start_level = 0; start_level < 2; start_level++) {
        for (unsigned int value = 0; value < 256; value++) {
            unsigned int level = start_level;
            uint16_t bits = 0;
            unsigned int parity = 0;

            for (unsigned int bit = 0; bit < 8; bit++) {
                const bool bit_value = (value >> bit) & 1u;
                parity ^= bit_value ? 1u : 0u;

                level ^= 1u;
                if (level) {
                    bits |= 1u << (bit * 2);
                }

                if (bit_value) {
                    level ^= 1u;
                }
                if (level) {
                    bits |= 1u << (bit * 2 + 1);
                }
            }

            bmc_byte[start_level][value] = (bmc_byte_t){
                .bits = bits,
                .level = (uint8_t)level,
                .parity = (uint8_t)parity,
            };
        }
    }

    for (unsigned int start_level = 0; start_level < 2; start_level++) {
        for (unsigned int parity = 0; parity < 2; parity++) {
            unsigned int level = start_level;
            uint8_t bits = 0;

            for (unsigned int bit = 0; bit < 3; bit++) {
                level ^= 1u;
                if (level) {
                    bits |= 1u << (bit * 2);
                    bits |= 1u << (bit * 2 + 1);
                }
            }

            level ^= 1u;
            if (level) {
                bits |= 1u << 6;
            }

            if (parity) {
                level ^= 1u;
            }
            if (level) {
                bits |= 1u << 7;
            }

            bmc_tail[start_level][parity] = bits;
        }
    }
}

static void append_half_bit(unsigned int level) {
    if (level) {
        build_words[spdif_word_index / 32] |= 1u << (spdif_word_index % 32);
    }
    spdif_word_index++;
}

static void append_preamble(spdif_preamble_t preamble) {
    uint8_t pattern = 0;

    switch (preamble) {
    case PREAMBLE_B:
        pattern = 0xe8;
        break;
    case PREAMBLE_M:
        pattern = 0xe2;
        break;
    case PREAMBLE_W:
        pattern = 0xe4;
        break;
    }

    if (current_level) {
        pattern = (uint8_t)~pattern;
    }

    for (int bit = 7; bit >= 0; bit--) {
        current_level = (pattern >> bit) & 1u;
        append_half_bit(current_level);
    }
}

static void append_bmc_bit(bool bit) {
    current_level ^= 1u;
    append_half_bit(current_level);

    if (bit) {
        current_level ^= 1u;
    }
    append_half_bit(current_level);
}

static void append_subframe(spdif_preamble_t preamble, int32_t sample) {
    // sample is 24-bit left-aligned in int32_t; bits 31..8 carry the audio,
    // bits 7..0 are ignored. The S/PDIF subframe carries audio bits LSB-first.
    uint32_t payload = ((uint32_t)sample) >> 8;
    unsigned int ones = 0;

    append_preamble(preamble);

    for (int bit = 0; bit < 24; bit++) {
        const bool value = (payload >> bit) & 1u;
        ones += value ? 1u : 0u;
        append_bmc_bit(value);
    }

    // V, U, and C are all zero for this first PCM test tone.
    append_bmc_bit(false);
    append_bmc_bit(false);
    append_bmc_bit(false);

    append_bmc_bit((ones & 1u) != 0);
}

static inline void put_bits16(uint32_t *lo, uint32_t *hi, unsigned int offset, uint16_t bits) {
    if (offset < 32) {
        *lo |= (uint32_t)bits << offset;
        if (offset > 16) {
            *hi |= (uint32_t)bits >> (32 - offset);
        }
    } else {
        *hi |= (uint32_t)bits << (offset - 32);
    }
}

static void encode_subframe_fast(uint32_t words[SPDIF_WORDS_PER_SUBFRAME],
                                 spdif_preamble_t preamble, int32_t sample, unsigned int *level) {
    uint8_t pattern = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;

    switch (preamble) {
    case PREAMBLE_B:
        pattern = 0xe8;
        break;
    case PREAMBLE_M:
        pattern = 0xe2;
        break;
    case PREAMBLE_W:
        pattern = 0xe4;
        break;
    }

    if (*level) {
        pattern = (uint8_t)~pattern;
    }

    for (int bit = 7; bit >= 0; bit--) {
        *level = (pattern >> bit) & 1u;
        if (*level) {
            lo |= 1u << (7 - bit);
        }
    }

    // 24 audio bits, LSB-first across three encoder bytes, taken from the
    // 24-bit-left-aligned int32 (audio MSB at bit 31, audio LSB at bit 8).
    const uint32_t raw = (uint32_t)sample;
    bmc_byte_t chunk = bmc_byte[*level][(raw >> 8) & 0xffu];
    put_bits16(&lo, &hi, 8, chunk.bits);
    *level = chunk.level;
    unsigned int parity = chunk.parity;

    chunk = bmc_byte[*level][(raw >> 16) & 0xffu];
    put_bits16(&lo, &hi, 24, chunk.bits);
    *level = chunk.level;
    parity ^= chunk.parity;

    chunk = bmc_byte[*level][(raw >> 24) & 0xffu];
    put_bits16(&lo, &hi, 40, chunk.bits);
    *level = chunk.level;
    parity ^= chunk.parity;

    hi |= (uint32_t)bmc_tail[*level][parity] << 24;
    *level = (hi >> 31) & 1u;

    words[0] = lo;
    words[1] = hi;
}

static bool read_usb_frame(int32_t *left, int32_t *right) {
    const unsigned int read = usb_ring_read;

    if (read == usb_ring_write) {
        *left = 0;
        *right = 0;
        usb_underrun_frames++;
        return false;
    }

    *left = usb_ring[read][0];
    *right = usb_ring[read][1];
    usb_ring_read = (read + 1) % USB_RING_FRAMES;
    return true;
}

static void encode_live_frame(uint32_t *words, unsigned int frame_index, unsigned int *level) {
    int32_t left = 0;
    int32_t right = 0;

    read_usb_frame(&left, &right);
    encode_subframe_fast(&words[0], frame_index == 0 ? PREAMBLE_B : PREAMBLE_M, left, level);
    encode_subframe_fast(&words[2], PREAMBLE_W, right, level);
}

static void copy_block(uint32_t *dst, const uint32_t *src) {
    for (size_t i = 0; i < SPDIF_WORDS_PER_BLOCK; i++) {
        dst[i] = src[i];
    }
}

static void build_dma_block(uint32_t *words, unsigned int *frame_index, unsigned int *level) {
    const spdif_mode_t mode = current_mode;

    if (mode == SPDIF_MODE_USB_AUDIO) {
        for (unsigned int frame = 0; frame < SPDIF_FRAMES_PER_BLOCK; frame++) {
            encode_live_frame(&words[frame * SPDIF_WORDS_PER_FRAME], *frame_index, level);
            *frame_index = (*frame_index + 1) % SPDIF_FRAMES_PER_BLOCK;
        }
        return;
    }

    copy_block(words, mode == SPDIF_MODE_TONE_1KHZ ? tone_words : silence_words);
}

static void start_dma_block(const uint32_t *words) {
    dma_channel_configure(spdif_dma_chan,
                          &spdif_dma_config,
                          &spdif_pio->txf[spdif_sm],
                          words,
                          SPDIF_WORDS_PER_BLOCK,
                          true);
}

static void build_block(uint32_t *words, bool tone) {
    for (size_t i = 0; i < SPDIF_WORDS_PER_BLOCK; i++) {
        words[i] = 0;
    }

    build_words = words;
    spdif_word_index = 0;
    current_level = 0;

    for (unsigned int frame = 0; frame < SPDIF_FRAMES_PER_BLOCK; frame++) {
        // sine_1khz_48k is 16-bit; promote to the encoder's 24-bit
        // left-aligned int32 by shifting up the same way live USB does.
        const int32_t sample = tone ? ((int32_t)sine_1khz_48k[frame % 48]) << 16 : 0;
        append_subframe(frame == 0 ? PREAMBLE_B : PREAMBLE_M, sample);
        append_subframe(PREAMBLE_W, sample);
    }
}

static void audio_core_main(void) {
    unsigned int live_frame_index = 0;
    unsigned int live_level = 0;
    unsigned int block = 0;

    build_dma_block(dma_words[block], &live_frame_index, &live_level);
    start_dma_block(dma_words[block]);
    block ^= 1u;

    while (true) {
        build_dma_block(dma_words[block], &live_frame_index, &live_level);
        dma_channel_wait_for_finish_blocking(spdif_dma_chan);
        start_dma_block(dma_words[block]);
        block ^= 1u;
    }
}

void spdif_start(unsigned int pin) {
    build_bmc_tables();
    build_block(tone_words, true);
    build_block(silence_words, false);

    spdif_pio = pio0;
    spdif_sm = pio_claim_unused_sm(spdif_pio, true);
    const unsigned int offset = pio_add_program(spdif_pio, &spdif_tx_program);

    pio_gpio_init(spdif_pio, pin);
    pio_sm_set_consecutive_pindirs(spdif_pio, spdif_sm, pin, 1, true);

    pio_sm_config config = spdif_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&config, pin, 1);
    sm_config_set_out_shift(&config, true, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / SPDIF_PIN_RATE_HZ);

    pio_sm_init(spdif_pio, spdif_sm, offset, &config);
    pio_sm_set_enabled(spdif_pio, spdif_sm, true);

    spdif_dma_chan = dma_claim_unused_channel(true);
    spdif_dma_config = dma_channel_get_default_config(spdif_dma_chan);
    channel_config_set_transfer_data_size(&spdif_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&spdif_dma_config, true);
    channel_config_set_write_increment(&spdif_dma_config, false);
    channel_config_set_dreq(&spdif_dma_config, pio_get_dreq(spdif_pio, spdif_sm, true));
    channel_config_set_high_priority(&spdif_dma_config, true);

    multicore_launch_core1(audio_core_main);
}

void spdif_set_mode(spdif_mode_t mode) {
    current_mode = mode;
}

spdif_mode_t spdif_get_mode(void) {
    return current_mode;
}

const char *spdif_mode_name(spdif_mode_t mode) {
    switch (mode) {
    case SPDIF_MODE_SILENCE:
        return "silence";
    case SPDIF_MODE_TONE_1KHZ:
        return "1khz-tone";
    case SPDIF_MODE_USB_AUDIO:
        return "usb-audio";
    default:
        return "unknown";
    }
}

unsigned int spdif_write_pcm(const int32_t *samples, unsigned int frame_count) {
    unsigned int written = 0;

    for (unsigned int i = 0; i < frame_count; i++) {
        const unsigned int write = usb_ring_write;
        const unsigned int next_write = (write + 1) % USB_RING_FRAMES;

        if (next_write == usb_ring_read) {
            break;
        }

        usb_ring[write][0] = samples[i * 2];
        usb_ring[write][1] = samples[i * 2 + 1];
        usb_ring_write = next_write;
        written++;
    }

    const unsigned int buffered = spdif_buffered_frames();
    if (buffered > usb_high_water_frames) {
        usb_high_water_frames = buffered;
    }

    return written;
}

unsigned int spdif_buffered_frames(void) {
    return buffered_frames_from(usb_ring_read, usb_ring_write);
}

void spdif_clear_usb_buffer(void) {
    usb_ring_read = 0;
    usb_ring_write = 0;
    usb_underrun_frames = 0;
    usb_high_water_frames = 0;
}

void spdif_take_usb_stats(spdif_usb_stats_t *stats) {
    if (!stats) {
        return;
    }

    stats->buffered_frames = spdif_buffered_frames();
    stats->high_water_frames = usb_high_water_frames;
    stats->underrun_frames = usb_underrun_frames;

    usb_high_water_frames = stats->buffered_frames;
    usb_underrun_frames = 0;
}
