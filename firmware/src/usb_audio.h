#ifndef PICOARC_USB_AUDIO_H
#define PICOARC_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void usb_audio_task(void);
bool usb_audio_is_streaming(void);
void usb_audio_set_cec_audio_status(uint8_t volume, bool muted);
void usb_audio_set_cec_mute_status(bool muted);

#endif
