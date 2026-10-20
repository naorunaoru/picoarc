#ifndef PICOARC_USB_AUDIO_H
#define PICOARC_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void usb_audio_task(void);
void usb_audio_prepare_connection(void);
bool usb_audio_is_streaming(void);
bool usb_audio_take_recovery_request(void);
void usb_audio_stop_streaming(void);
void usb_audio_set_cec_audio_status(uint8_t volume, bool muted, bool notify_host);
void usb_audio_set_cec_mute_status(bool muted, bool notify_host);

#if PICOARC_USB_DIAGNOSTICS
bool usb_audio_take_hdmi_recovery_request(void);
enum {
    PICOARC_DIAGNOSTICS_DDC = 4,
    PICOARC_DIAGNOSTICS_HPD = 5,
    PICOARC_DIAGNOSTICS_CEC = 6,
    PICOARC_DIAGNOSTICS_CEC_RX = 7,
};
uint8_t usb_audio_take_isolated_recovery_request(void);
void usb_audio_trace_state(void);
#endif

#endif
