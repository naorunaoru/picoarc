#include "spdif.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "picoarc_log.h"
#include "spdif.pio.h"

enum {
    // Each S/PDIF audio frame is 2 subframes × 32 bits × 2 half-bits = 128
    // half-bits, and the PIO outputs 1 half-bit per cycle, so the PIO clock
    // is 128× the audio sample rate (6.144 MHz at 48 kHz, 12.288 MHz at 96 kHz).
    SPDIF_HALF_BITS_PER_FRAME = 128,
    SPDIF_DEFAULT_SAMPLE_RATE_HZ = 48000,
    SPDIF_FRAMES_PER_BLOCK = 192,
    SPDIF_WORDS_PER_SUBFRAME = 2,
    SPDIF_WORDS_PER_FRAME = 4,
    SPDIF_WORDS_PER_BLOCK = SPDIF_FRAMES_PER_BLOCK * SPDIF_WORDS_PER_FRAME,
    // One slot stays empty so read == write can unambiguously mean empty.
    USB_RING_FRAMES = 2048,
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

static uint32_t silence_words[SPDIF_WORDS_PER_BLOCK];
static uint32_t *build_words;
static size_t spdif_word_index;
static unsigned int current_level;
static PIO spdif_pio;
static unsigned int spdif_sm;
static unsigned int spdif_dma_chan[2];
static uint32_t spdif_dma_mask;
static dma_channel_config spdif_dma_config[2];
static unsigned int live_frame_index;
static unsigned int live_level;
static volatile spdif_mode_t current_mode = SPDIF_MODE_OFF;
static volatile spdif_stream_format_t current_stream_format = SPDIF_STREAM_FORMAT_PCM;
// Samples are stored 24-bit-left-aligned in int32_t (audio MSB at bit 31,
// audio LSB at bit 8). 16-bit USB input is shifted left 16 on ingest, 24-bit
// USB input is unpacked from its 3-byte little-endian form. Either way the
// S/PDIF encoder sees the same internal representation.
static int32_t usb_ring[USB_RING_FRAMES][2];
static volatile unsigned int usb_ring_read;
static volatile unsigned int usb_ring_write;
static volatile unsigned int usb_underrun_frames;
static volatile unsigned int usb_high_water_frames;
static volatile unsigned int usb_low_water_frames;
static volatile unsigned int spdif_dma_late_blocks;
static volatile unsigned int spdif_dma_rearm_races;
static volatile unsigned int spdif_dma_max_build_us;
static volatile unsigned int spdif_dma_sequence_errors;
static volatile unsigned int spdif_pio_stall_events;
static volatile uint32_t current_sample_rate = SPDIF_DEFAULT_SAMPLE_RATE_HZ;
static volatile unsigned int current_sample_bits = 16;
static bmc_byte_t bmc_byte[2][256];
static uint8_t bmc_tail[2][2][8];
static uint32_t dma_words[2][SPDIF_WORDS_PER_BLOCK];
static unsigned int expected_dma_block;

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
        for (unsigned int payload_parity = 0; payload_parity < 2; payload_parity++) {
            for (unsigned int vuc_bits = 0; vuc_bits < 8; vuc_bits++) {
                unsigned int level = start_level;
                unsigned int parity = payload_parity;
                uint8_t bits = 0;

                for (unsigned int bit = 0; bit < 3; bit++) {
                    const bool value = (vuc_bits >> bit) & 1u;
                    parity ^= value ? 1u : 0u;

                    level ^= 1u;
                    if (level) {
                        bits |= 1u << (bit * 2);
                    }

                    if (value) {
                        level ^= 1u;
                    }
                    if (level) {
                        bits |= 1u << (bit * 2 + 1);
                    }
                }

                const bool parity_bit = (parity & 1u) != 0;
                level ^= 1u;
                if (level) {
                    bits |= 1u << 6;
                }

                if (parity_bit) {
                    level ^= 1u;
                }
                if (level) {
                    bits |= 1u << 7;
                }

                bmc_tail[start_level][payload_parity][vuc_bits] = bits;
            }
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

static void append_subframe(spdif_preamble_t preamble, int32_t sample, bool channel_status) {
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

    // V and U are zero. C carries channel status, bit-by-bit across the
    // 192-frame block.
    append_bmc_bit(false);
    append_bmc_bit(false);
    append_bmc_bit(channel_status);
    ones += channel_status ? 1u : 0u;

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
                                 spdif_preamble_t preamble, int32_t sample,
                                 bool channel_status, unsigned int *level) {
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

    const unsigned int vuc_bits = channel_status ? 0x04u : 0x00u;
    hi |= (uint32_t)bmc_tail[*level][parity][vuc_bits] << 24;
    *level = (hi >> 31) & 1u;

    words[0] = lo;
    words[1] = hi;
}

static uint8_t channel_status_sample_rate_code(uint32_t rate_hz) {
    // IEC 60958 consumer channel-status byte 3, bits 0..3. Bit zero of
    // the returned nibble is transmitted as channel-status bit 24.
    switch (rate_hz) {
    case 32000:
        return 0x3;
    case 44100:
        return 0x0;
    case 48000:
        return 0x2;
    case 88200:
        return 0x8;
    case 96000:
        return 0xa;
    default:
        return 0x1; // Sample frequency not indicated.
    }
}

static uint8_t channel_status_original_rate_code(uint32_t rate_hz) {
    // IEC 60958 consumer channel-status byte 4, bits 4..7.
    switch (rate_hz) {
    case 32000:
        return 0xc;
    case 44100:
        return 0xf;
    case 48000:
        return 0xd;
    case 88200:
        return 0x7;
    case 96000:
        return 0x5;
    default:
        return 0x0; // Original sample frequency not indicated.
    }
}

static uint8_t channel_status_word_length(void) {
    if (current_stream_format != SPDIF_STREAM_FORMAT_PCM) {
        return 0;
    }

    switch (current_sample_bits) {
    case 16:
        return 0x02; // 16 bits with a maximum word length of 20 bits.
    case 20:
        return 0x0a; // 20 bits with a maximum word length of 20 bits.
    case 24:
        return 0x0b; // 24 bits with a maximum word length of 24 bits.
    default:
        return 0; // Word length not indicated.
    }
}

static bool channel_status_bit(unsigned int frame_index,
                               spdif_stream_format_t format) {
    uint8_t byte = 0;

    switch (frame_index / 8) {
    case 0:
        // Consumer channel status. Byte 0 bit 1 distinguishes linear PCM
        // from IEC 61937 non-audio data.
        byte = format == SPDIF_STREAM_FORMAT_IEC61937 ? 0x02 : 0x00;
        break;
    case 3:
        byte = channel_status_sample_rate_code(current_sample_rate);
        break;
    case 4:
        byte = channel_status_word_length() |
               (uint8_t)(channel_status_original_rate_code(current_sample_rate) << 4);
        break;
    default:
        break;
    }

    return ((byte >> (frame_index % 8)) & 1u) != 0;
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

    const unsigned int buffered = buffered_frames_from(usb_ring_read, usb_ring_write);
    if (buffered < usb_low_water_frames) {
        usb_low_water_frames = buffered;
    }
    return true;
}

static void encode_live_frame(uint32_t *words, unsigned int frame_index, unsigned int *level) {
    int32_t left = 0;
    int32_t right = 0;
    const bool c_bit = channel_status_bit(frame_index, current_stream_format);

    read_usb_frame(&left, &right);
    encode_subframe_fast(&words[0], frame_index == 0 ? PREAMBLE_B : PREAMBLE_M, left, c_bit, level);
    encode_subframe_fast(&words[2], PREAMBLE_W, right, c_bit, level);
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

    if (mode == SPDIF_MODE_OFF) {
        for (size_t i = 0; i < SPDIF_WORDS_PER_BLOCK; i++) {
            words[i] = 0;
        }
        return;
    }

    copy_block(words, silence_words);
}

static void build_silence_block(uint32_t *words) {
    for (size_t i = 0; i < SPDIF_WORDS_PER_BLOCK; i++) {
        words[i] = 0;
    }

    build_words = words;
    spdif_word_index = 0;
    current_level = 0;

    for (unsigned int frame = 0; frame < SPDIF_FRAMES_PER_BLOCK; frame++) {
        const bool c_bit = channel_status_bit(frame, current_stream_format);
        append_subframe(frame == 0 ? PREAMBLE_B : PREAMBLE_M, 0, c_bit);
        append_subframe(PREAMBLE_W, 0, c_bit);
    }
}

void spdif_start(unsigned int pin) {
    build_bmc_tables();
    build_silence_block(silence_words);

    spdif_pio = pio0;
    spdif_sm = pio_claim_unused_sm(spdif_pio, true);
    const unsigned int offset = pio_add_program(spdif_pio, &spdif_tx_program);

    pio_gpio_init(spdif_pio, pin);
    // The ARC output network is 330 ohms in series with a 54.5 ohm shunt,
    // drawing about 8.6 mA while GP2 is high. RP2040 pads reset to 4 mA with
    // slow slew, which leaves the 12.288 MHz (96 kHz audio) carrier marginal.
    // Use the pad's 12 mA setting for voltage headroom and fast slew for clean
    // half-bit edges; the 330 ohm source resistor still limits current.
    gpio_disable_pulls(pin);
    gpio_set_input_enabled(pin, false);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    pio_sm_set_consecutive_pindirs(spdif_pio, spdif_sm, pin, 1, true);

    pio_sm_config config = spdif_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&config, pin, 1);
    sm_config_set_out_shift(&config, true, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config,
                         (float)clock_get_hz(clk_sys) /
                             (SPDIF_DEFAULT_SAMPLE_RATE_HZ * SPDIF_HALF_BITS_PER_FRAME));

    pio_sm_init(spdif_pio, spdif_sm, offset, &config);
    pio_sm_set_enabled(spdif_pio, spdif_sm, true);

    spdif_dma_chan[0] = dma_claim_unused_channel(true);
    spdif_dma_chan[1] = dma_claim_unused_channel(true);
    spdif_dma_mask = (1u << spdif_dma_chan[0]) | (1u << spdif_dma_chan[1]);
    dma_hw->intr = spdif_dma_mask;
    for (unsigned int block = 0; block < 2; block++) {
        spdif_dma_config[block] = dma_channel_get_default_config(spdif_dma_chan[block]);
        channel_config_set_transfer_data_size(&spdif_dma_config[block], DMA_SIZE_32);
        channel_config_set_read_increment(&spdif_dma_config[block], true);
        channel_config_set_write_increment(&spdif_dma_config[block], false);
        channel_config_set_dreq(&spdif_dma_config[block],
                                pio_get_dreq(spdif_pio, spdif_sm, true));
        channel_config_set_high_priority(&spdif_dma_config[block], true);
        channel_config_set_chain_to(&spdif_dma_config[block],
                                    spdif_dma_chan[block ^ 1u]);
    }

    live_frame_index = 0;
    live_level = 0;
    expected_dma_block = 0;
    build_dma_block(dma_words[0], &live_frame_index, &live_level);
    build_dma_block(dma_words[1], &live_frame_index, &live_level);

    // Channel 0 and channel 1 trigger each other at exact block boundaries.
    // The cooperative task only refills the channel that just completed, so
    // TinyUSB processing cannot insert a gap between DMA blocks.
    dma_channel_configure(spdif_dma_chan[1],
                          &spdif_dma_config[1],
                          &spdif_pio->txf[spdif_sm],
                          dma_words[1],
                          SPDIF_WORDS_PER_BLOCK,
                          false);
    dma_channel_configure(spdif_dma_chan[0],
                          &spdif_dma_config[0],
                          &spdif_pio->txf[spdif_sm],
                          dma_words[0],
                          SPDIF_WORDS_PER_BLOCK,
                          true);

    // Enabling the PIO before its DMA source is armed produces an expected
    // startup stall. Clear it so subsequent events represent real carrier
    // interruptions.
    spdif_pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + spdif_sm);
}

void spdif_task(void) {
    const uint32_t pio_stall_mask =
        1u << (PIO_FDEBUG_TXSTALL_LSB + spdif_sm);
    if (spdif_pio->fdebug & pio_stall_mask) {
        spdif_pio->fdebug = pio_stall_mask;
        spdif_pio_stall_events++;
    }

    const uint32_t completed = dma_hw->intr & spdif_dma_mask;
    if (!completed) {
        return;
    }
    dma_hw->intr = completed;

    unsigned int completed_block = 2;
    if (completed == (1u << spdif_dma_chan[0])) {
        completed_block = 0;
    } else if (completed == (1u << spdif_dma_chan[1])) {
        completed_block = 1;
    }
    if (completed_block >= 2 || completed_block != expected_dma_block) {
        spdif_dma_sequence_errors++;
    }
    if (completed_block < 2) {
        expected_dma_block = completed_block ^ 1u;
    }

    for (unsigned int block = 0; block < 2; block++) {
        const uint32_t channel_mask = 1u << spdif_dma_chan[block];
        if (!(completed & channel_mask)) {
            continue;
        }

        // TRANS_COUNT reads as zero while an inactive channel is waiting to
        // be chained, even after its reload value has been programmed. Use
        // the raw completion flags to distinguish that armed state from a new
        // completion, and never overwrite a buffer while its channel is live.
        if (dma_channel_is_busy(spdif_dma_chan[block])) {
            spdif_dma_late_blocks++;
            continue;
        }

        const uint32_t build_started_us = time_us_32();
        build_dma_block(dma_words[block], &live_frame_index, &live_level);
        const uint32_t build_us = time_us_32() - build_started_us;
        if (build_us > spdif_dma_max_build_us) {
            spdif_dma_max_build_us = build_us;
        }

        // At 96 kHz, the other 192-frame block gives us exactly 2 ms to
        // rebuild and rearm this channel. The pre-build busy check above does
        // not catch the channel being chained while build_dma_block() runs.
        // Record that deadline miss before preserving the existing behavior;
        // writing READ_ADDR while active can cause the audible discontinuity
        // this diagnostic is intended to confirm.
        const uint32_t completions_during_build =
            dma_hw->intr & spdif_dma_mask;
        if (completions_during_build ||
            dma_channel_is_busy(spdif_dma_chan[block])) {
            spdif_dma_rearm_races++;
        }
        dma_channel_set_read_addr(spdif_dma_chan[block], dma_words[block], false);
    }

    if (!dma_channel_is_busy(spdif_dma_chan[0]) &&
        !dma_channel_is_busy(spdif_dma_chan[1])) {
        // Core 1 was delayed for longer than two complete blocks. Restart the
        // chain and expose the incident through the existing diagnostics.
        spdif_dma_late_blocks++;
        dma_start_channel_mask(1u << spdif_dma_chan[0]);
    }
}

void spdif_set_mode(spdif_mode_t mode) {
    const spdif_mode_t previous = current_mode;
    current_mode = mode;
    if (previous != mode) {
        printf("spdif: mode +%llums %s -> %s buf=%u\n",
               (unsigned long long)(time_us_64() / 1000),
               spdif_mode_name(previous),
               spdif_mode_name(mode),
               spdif_buffered_frames());
    }
}

void spdif_set_sample_rate(uint32_t rate_hz) {
    if (!spdif_pio) {
        // Called before spdif_start() — caller booted us out of order. Drop
        // the request rather than writing into ROM at NULL+0xC8.
        return;
    }

    current_sample_rate = rate_hz;
    build_silence_block(silence_words);

    const float divider = (float)clock_get_hz(clk_sys) /
                          (float)(rate_hz * SPDIF_HALF_BITS_PER_FRAME);
    pio_sm_set_enabled(spdif_pio, spdif_sm, false);
    pio_sm_set_clkdiv(spdif_pio, spdif_sm, divider);
    pio_sm_clkdiv_restart(spdif_pio, spdif_sm);
    pio_sm_set_enabled(spdif_pio, spdif_sm, true);
}

spdif_mode_t spdif_get_mode(void) {
    return current_mode;
}

void spdif_set_stream_format(spdif_stream_format_t format,
                             unsigned int sample_bits) {
    current_stream_format = format;
    current_sample_bits = sample_bits;
    build_silence_block(silence_words);
}

spdif_stream_format_t spdif_get_stream_format(void) {
    return current_stream_format;
}

const char *spdif_mode_name(spdif_mode_t mode) {
    switch (mode) {
    case SPDIF_MODE_OFF:
        return "off";
    case SPDIF_MODE_SILENCE:
        return "silence";
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
    usb_low_water_frames = USB_RING_FRAMES;
    spdif_dma_late_blocks = 0;
    spdif_dma_rearm_races = 0;
    spdif_dma_max_build_us = 0;
    spdif_dma_sequence_errors = 0;
    spdif_pio_stall_events = 0;
}

void spdif_take_usb_stats(spdif_usb_stats_t *stats) {
    if (!stats) {
        return;
    }

    stats->buffered_frames = spdif_buffered_frames();
    stats->high_water_frames = usb_high_water_frames;
    stats->low_water_frames = usb_low_water_frames == USB_RING_FRAMES ?
                                  stats->buffered_frames :
                                  usb_low_water_frames;
    stats->underrun_frames = usb_underrun_frames;
    stats->dma_late_blocks = spdif_dma_late_blocks;
    stats->dma_rearm_races = spdif_dma_rearm_races;
    stats->dma_max_build_us = spdif_dma_max_build_us;
    stats->dma_sequence_errors = spdif_dma_sequence_errors;
    stats->pio_stall_events = spdif_pio_stall_events;

    usb_high_water_frames = stats->buffered_frames;
    usb_low_water_frames = stats->buffered_frames;
    usb_underrun_frames = 0;
    spdif_dma_late_blocks = 0;
    spdif_dma_rearm_races = 0;
    spdif_dma_max_build_us = 0;
    spdif_dma_sequence_errors = 0;
    spdif_pio_stall_events = 0;
}
