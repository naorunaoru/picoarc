#ifndef PICOARC_SPDIF_H
#define PICOARC_SPDIF_H

#include <stdint.h>

typedef enum {
    SPDIF_MODE_OFF,
    SPDIF_MODE_SILENCE,
    SPDIF_MODE_TONE_1KHZ,
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
// PIO clkdiv changes. The host is required by UAC2 to drop alt=0 before
// SET CUR on the clock source, so streaming is stopped when this is called.
void spdif_set_sample_rate(uint32_t rate_hz);
// samples is interleaved L/R 24-bit audio left-aligned in int32_t: the audio
// MSB sits at bit 31 and the audio LSB at bit 8. Bits 7..0 are ignored. 16-bit
// callers shift their value left by 16 before calling.
unsigned int spdif_write_pcm(const int32_t *samples, unsigned int frame_count);
unsigned int spdif_buffered_frames(void);
void spdif_clear_usb_buffer(void);
void spdif_take_usb_stats(spdif_usb_stats_t *stats);

#endif
