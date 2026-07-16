#ifndef PICOARC_USB_AUDIO_H
#define PICOARC_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void usb_audio_task(void);
bool usb_audio_is_streaming(void);
bool usb_audio_take_recovery_request(void);
void usb_audio_stop_streaming(void);
void usb_audio_set_cec_audio_status(uint8_t volume, bool muted, bool notify_host);
void usb_audio_set_cec_mute_status(bool muted, bool notify_host);

#endif
