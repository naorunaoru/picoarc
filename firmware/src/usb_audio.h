#ifndef PICOARC_USB_AUDIO_H
#define PICOARC_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void usb_audio_task(void);
void usb_audio_set_cec_audio_status(uint8_t volume, bool muted);

#endif
