#ifndef PICOARC_SPDIF_H
#define PICOARC_SPDIF_H

#include <stdint.h>

typedef enum {
    SPDIF_MODE_SILENCE,
    SPDIF_MODE_TONE_1KHZ,
    SPDIF_MODE_USB_AUDIO,
} spdif_mode_t;

typedef struct {
    unsigned int buffered_frames;
    unsigned int high_water_frames;
    unsigned int underrun_frames;
} spdif_usb_stats_t;

void spdif_start(unsigned int pin);
void spdif_set_mode(spdif_mode_t mode);
spdif_mode_t spdif_get_mode(void);
const char *spdif_mode_name(spdif_mode_t mode);
unsigned int spdif_write_pcm(const int16_t *samples, unsigned int frame_count);
unsigned int spdif_buffered_frames(void);
void spdif_clear_usb_buffer(void);
void spdif_take_usb_stats(spdif_usb_stats_t *stats);

#endif
