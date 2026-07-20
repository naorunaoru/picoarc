#ifndef PICOARC_ARC_H
#define PICOARC_ARC_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_caps.h"

void arc_init(unsigned int cec_pin, unsigned int hdmi_5v_pin);
void arc_task(void);
bool arc_hdmi_connected(void);
bool arc_cec_scan_complete(void);
bool arc_cec_any_device_found(void);
bool arc_cec_audio_system_found(void);
bool arc_system_audio_enabled(void);
bool arc_is_initiated(void);
bool arc_audio_caps_ready(void);
bool arc_ready_for_usb(void);
void arc_get_audio_caps_snapshot(picoarc_audio_caps_t *caps);
bool arc_audio_format_supported(uint8_t alt, uint32_t sample_rate);
bool arc_audio_format_supported_quiet(uint8_t alt, uint32_t sample_rate);
void arc_request_volume_sync(uint8_t volume);
void arc_request_mute_sync(bool muted);

#endif
