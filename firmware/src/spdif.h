#ifndef PICOARC_SPDIF_H
#define PICOARC_SPDIF_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SPDIF_MODE_OFF,
    SPDIF_MODE_SILENCE,
    SPDIF_MODE_USB_AUDIO,
} spdif_mode_t;

typedef enum {
    SPDIF_STREAM_FORMAT_PCM,
    SPDIF_STREAM_FORMAT_IEC61937,
} spdif_stream_format_t;

typedef struct {
    unsigned int buffered_frames;
    unsigned int high_water_frames;
    unsigned int low_water_frames;
    unsigned int underrun_frames;
    unsigned int dma_late_blocks;
} spdif_usb_stats_t;

void spdif_start(unsigned int pin);
void spdif_set_mode(spdif_mode_t mode);
spdif_mode_t spdif_get_mode(void);
const char *spdif_mode_name(spdif_mode_t mode);
void spdif_set_stream_format(spdif_stream_format_t format);
spdif_stream_format_t spdif_get_stream_format(void);
// Switch the PIO output clock to the given sample rate. The encoder block
// layout (192 stereo frames per DMA block) is sample-rate-agnostic — only the
// PIO clkdiv changes. The USB audio path clears and refills its buffers around
// USB SET_CUR rate changes.
void spdif_set_sample_rate(uint32_t rate_hz);
#if PICOARC_UAC_VERSION == 2
// Adapt the S/PDIF carrier to the USB host clock. Positive values consume the
// receive ring faster. The divider is updated once per 192-frame DMA cycle.
void spdif_set_rate_adjustment_ppm(int32_t adjustment_ppm);
// Ring occupancy sampled by the encoder at a consistent point immediately
// after it stages the next 192-frame DMA block.
bool spdif_adaptive_buffered_frames(unsigned int *frames);
#endif
// samples is interleaved L/R 24-bit audio left-aligned in int32_t: the audio
// MSB sits at bit 31 and the audio LSB at bit 8. Bits 7..0 are ignored. 16-bit
// callers shift their value left by 16 before calling.
unsigned int spdif_write_pcm(const int32_t *samples, unsigned int frame_count);
unsigned int spdif_buffered_frames(void);
void spdif_clear_usb_buffer(void);
void spdif_take_usb_stats(spdif_usb_stats_t *stats);

#if PICOARC_USB_DIAGNOSTICS
typedef struct {
    uint32_t dma_blocks;
    uint32_t dma_remaining;
    uint32_t dma_ctrl;
    uint32_t pio_fdebug;
} spdif_diagnostics_t;

// Read counters/registers without clearing them or waiting for core 1.
void spdif_get_diagnostics(spdif_diagnostics_t *diagnostics);

typedef struct {
    uint32_t version;
    uint32_t gpio_ctrl;
    uint32_t gpio_status;
    uint32_t pio_ctrl;
    uint32_t pio_clkdiv;
    uint32_t pio_execctrl;
    uint32_t pio_shiftctrl;
    uint32_t pio_pinctrl;
    uint32_t pio_addr;
    uint32_t pio_fstat;
    uint32_t dma_read_addr;
    uint32_t stream_format;
    uint32_t encoded_nonzero_frames;
    uint32_t encoder_table_hash;
    uint32_t dma_block_hash;
    uint32_t silence_block_hash;
} spdif_output_diagnostics_t;

// Registers and hashes are sampled without stopping the encoder; DMA block
// hashes can cover concurrent writes and are not an atomic wire capture.
void spdif_get_output_diagnostics(spdif_output_diagnostics_t *diagnostics);

enum { SPDIF_DIAGNOSTIC_BLOCK_WORDS = 192 * 4 };
typedef struct {
    uint32_t version;
    uint32_t stable;
    uint32_t dma_blocks_before;
    uint32_t dma_blocks_after;
    uint32_t block_index;
    uint32_t dma_remaining_before;
    uint32_t dma_remaining_after;
    uint32_t spdif_mode;
    uint32_t stream_format;
    uint32_t pio_clkdiv;
    uint32_t pio_instruction;
    uint32_t gpio_pad;
    uint32_t clk_sys_hz;
    uint32_t uptime_ms;
    uint32_t reserved;
    uint32_t word_count;
    uint32_t words[SPDIF_DIAGNOSTIC_BLOCK_WORDS];
} spdif_block_diagnostics_t;

// Copy the buffer being consumed by DMA without stopping either core. A
// changing DMA block invalidates the copy; callers must check stable.
void spdif_get_block_diagnostics(spdif_block_diagnostics_t *diagnostics);

typedef struct {
    uint32_t requested;
    uint32_t completed;
    uint32_t last_uptime_ms;
    uint32_t reserved;
} spdif_restart_diagnostics_t;

// Schedule a transmitter-only restart on core 1. USB, ARC and received audio
// buffers remain intact; completion is reported asynchronously.
void spdif_request_output_restart(void);
void spdif_get_restart_diagnostics(spdif_restart_diagnostics_t *diagnostics);
#endif

#endif
