#ifndef PICOARC_REALTIME_H
#define PICOARC_REALTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_caps.h"

typedef struct {
    unsigned int size_bytes;
    unsigned int high_water_bytes;
    unsigned int free_bytes;
    bool canary_ok;
} realtime_core1_stack_stats_t;

// Initialize the shared control boundary, launch core 1, and wait until its
// TinyUSB and S/PDIF services are ready.
void realtime_start(unsigned int spdif_pin);

// Report the deepest observed use of the core-1 stack. The bottom 32 bytes
// are a canary: canary_ok becomes false if the stack reaches that reserve.
// Call from core 1 so the measurement includes the caller's live stack use.
void realtime_core1_stack_stats(realtime_core1_stack_stats_t *stats);

// Core 0 -> core 1 controls. TinyUSB and USB-audio functions are intentionally
// hidden behind this interface so they have a single-core owner.
void realtime_set_usb_attached(bool attached);
void realtime_set_audio_caps(const picoarc_audio_caps_t *caps);
void realtime_reset_audio_name(void);
void realtime_set_audio_name(const char *name);
void realtime_set_cec_audio_status(uint8_t volume, bool muted, bool notify_host);
void realtime_set_cec_mute_status(bool muted, bool notify_host);

// Core-1-owned state snapshots for the core-0 adapter/CEC state machines.
bool realtime_usb_attached(void);
bool realtime_usb_mounted(void);
bool realtime_usb_streaming(void);
unsigned int realtime_spdif_buffered_frames(void);
bool realtime_take_usb_recovery_request(void);

// Core 1 -> core 0 control requests produced by USB callbacks. Latest-value
// mailboxes intentionally coalesce rapid host volume changes.
bool realtime_take_bootsel_reset_request(void);
void realtime_post_volume_request(uint8_t volume);
void realtime_post_mute_request(bool muted);
bool realtime_take_volume_request(uint8_t *volume);
bool realtime_take_mute_request(bool *muted);

#endif
