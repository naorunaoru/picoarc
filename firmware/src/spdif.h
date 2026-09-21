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
    unsigned int dma_build_deadline_misses;
    unsigned int dma_max_build_us;
    unsigned int adaptive_skipped_updates;
    unsigned int pio_stall_events;
} spdif_usb_stats_t;

void spdif_start(unsigned int pin);
// Build and publish the next owned DMA buffer from the core-1 loop. A
// high-priority completion IRQ performs the boundary-critical handoff.
void spdif_task(void);
void spdif_set_mode(spdif_mode_t mode);
spdif_mode_t spdif_get_mode(void);
const char *spdif_mode_name(spdif_mode_t mode);
// Set the IEC 60958 channel-status format. sample_bits is used for linear PCM
// word-length indication and is ignored for IEC 61937 payloads.
void spdif_set_stream_format(spdif_stream_format_t format,
                             unsigned int sample_bits);
spdif_stream_format_t spdif_get_stream_format(void);
// Switch the PIO output clock to the given sample rate. The encoder block
// layout (192 stereo frames per DMA block) is sample-rate-agnostic — only the
// PIO clkdiv changes. The USB audio path clears and refills its buffers around
// USB SET_CUR rate changes.
void spdif_set_sample_rate(uint32_t rate_hz);
#if PICOARC_UAC_VERSION == 2
// Adapt the S/PDIF carrier to the USB host clock. Positive values consume the
// receive ring faster. The cooperative task applies at most one update for
// each observed 192-frame DMA completion and counts completions it skips.
void spdif_set_rate_adjustment_ppm(int32_t adjustment_ppm);
// Ring occupancy sampled by the encoder at a consistent point immediately
// after it stages the next 192-frame DMA block.
bool spdif_adaptive_buffered_frames(unsigned int *frames);
#endif
// samples is interleaved L/R 24-bit audio left-aligned in int32_t: the audio
// MSB sits at bit 31 and the audio LSB at bit 8. Bits 7..0 are ignored. 16-bit
// 16-bit callers should use defined multiplication by 65536 (see
// pcm16_to_internal()) rather than left-shifting a negative signed value.
unsigned int spdif_write_pcm(const int32_t *samples, unsigned int frame_count);
unsigned int spdif_buffered_frames(void);
void spdif_clear_usb_buffer(void);
void spdif_take_usb_stats(spdif_usb_stats_t *stats);

#endif
