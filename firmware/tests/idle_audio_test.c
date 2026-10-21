// Exercise the real USB callbacks and task with a fake clock and audio hardware.
#include <assert.h>
#include <stdio.h>

#include "arc.h"
#include "picoarc_config.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_audio.h"

static uint64_t now_us = UINT64_C(0xffffffff) - 500000;
static spdif_mode_t mode;
static spdif_stream_format_t format;
static unsigned int buffered_frames;
static unsigned int underrun_frames;
static bool supported = true;

uint8_t arc_audio_rate_mask(uint8_t format) {
    (void)format;
    return PICOARC_AUDIO_USB_RATE_MASK;
}
void pico_get_unique_board_id_string(char *id, size_t len) {
    snprintf(id, len, "test");
}
uint64_t time_us_64(void) { return now_us; }
void spdif_set_mode(spdif_mode_t value) { mode = value; }
void spdif_set_stream_format(spdif_stream_format_t value) { format = value; }
void spdif_set_sample_rate(uint32_t rate) { (void)rate; }
unsigned int spdif_buffered_frames(void) { return buffered_frames; }
void spdif_clear_usb_buffer(void) { buffered_frames = 0; }
unsigned int spdif_write_pcm(const int32_t *samples, unsigned int frames) {
    (void)samples;
    buffered_frames += frames;
    return frames;
}
void spdif_take_usb_stats(spdif_usb_stats_t *stats) {
    *stats = (spdif_usb_stats_t){.underrun_frames = underrun_frames};
    underrun_frames = 0;
}
bool arc_audio_format_supported_quiet(uint8_t alt, uint32_t rate) {
    (void)alt; (void)rate;
    return supported;
}
bool arc_audio_format_supported(uint8_t alt, uint32_t rate) {
    return arc_audio_format_supported_quiet(alt, rate);
}
void arc_request_mute_sync(bool muted) { (void)muted; }
void arc_request_volume_sync(uint8_t volume) { (void)volume; }
uint16_t tud_audio_n_available(uint8_t func) { (void)func; return 0; }
uint16_t tud_audio_n_read(uint8_t func, void *buffer, uint16_t size) {
    (void)func; (void)buffer; (void)size;
    assert(false); // No USB packets are queued by this fixture.
    return 0;
}
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t port,
        const tusb_control_request_t *request, void *data, uint16_t len) {
    (void)port; (void)request; (void)data; (void)len;
    return true;
}
#if PICOARC_UAC_VERSION == 1
bool tud_audio_n_fb_set(uint8_t func, uint32_t feedback) {
    (void)func; (void)feedback;
    return true;
}
#else
void spdif_set_rate_adjustment_ppm(int32_t adjustment) { (void)adjustment; }
bool spdif_adaptive_buffered_frames(unsigned int *frames) {
    *frames = buffered_frames;
    return true;
}
bool tud_audio_int_n_write(uint8_t func, const audio_interrupt_data_t *data) {
    (void)func; (void)data;
    return true;
}
#endif

static void select_alt(uint8_t alt) {
    const tusb_control_request_t request = {
        .wIndex = ITF_NUM_AUDIO_STREAMING, .wValue = alt,
    };
    assert(tud_audio_set_itf_cb(0, &request));
}

static void close_stream(void) {
    const tusb_control_request_t request = {
        .wIndex = ITF_NUM_AUDIO_STREAMING, .wValue = 0,
    };
    assert(tud_audio_set_itf_close_ep_cb(0, &request));
    // TinyUSB calls both callbacks when closing an active endpoint.
    select_alt(0);
    assert(!usb_audio_is_streaming());
    assert(format == SPDIF_STREAM_FORMAT_PCM);
    assert(mode == (PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS == 0 ?
                    SPDIF_MODE_OFF : SPDIF_MODE_SILENCE));
}

int main(void) {
    usb_descriptors_prepare_audio();
    // Boot and redundant alt-zero requests must not arm a finite timeout.
    usb_audio_stop_streaming();
    select_alt(0);
    usb_audio_task();
    assert(mode == (PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS == -1 ?
                    SPDIF_MODE_SILENCE : SPDIF_MODE_OFF));

    select_alt(PICOARC_AUDIO_ALT_PCM_16);
    assert(usb_audio_is_streaming());
    assert(mode == SPDIF_MODE_SILENCE);
    close_stream();

#if PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS > 0
    const uint64_t timeout_us = (uint64_t)PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS * 1000000u;
    const uint64_t first_close_us = now_us;
    now_us += timeout_us - 1;
    usb_audio_task();
    assert(mode == SPDIF_MODE_SILENCE);
    // A duplicate close just before expiry must not restart the countdown.
    select_alt(0);
    now_us++;
    usb_audio_task();
    assert(mode == SPDIF_MODE_OFF);
    select_alt(0);
    assert(mode == SPDIF_MODE_OFF);
    assert(now_us == first_close_us + timeout_us);

    // Reopening cancels expiry even if no packets arrive yet.
    select_alt(PICOARC_AUDIO_ALT_PCM_16);
    close_stream();
    now_us += timeout_us / 2;
    select_alt(PICOARC_AUDIO_ALT_PCM_24);
    now_us += timeout_us;
    usb_audio_task();
    assert(mode == SPDIF_MODE_SILENCE);
    close_stream();
    now_us += timeout_us - 1;
    usb_audio_task();
    assert(mode == SPDIF_MODE_SILENCE);
    now_us++;
    usb_audio_task();
    assert(mode == SPDIF_MODE_OFF);
#else
    now_us += UINT64_C(86400) * 1000000;
    usb_audio_task();
    assert(mode == (PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS == -1 ?
                    SPDIF_MODE_SILENCE : SPDIF_MODE_OFF));
#endif

    // An active stream stays on the carrier through underrun and refill,
    // including when idle policy is immediate standby or has already expired.
    select_alt(PICOARC_AUDIO_ALT_PCM_16);
    usb_audio_task();
    buffered_frames = 1024;
    usb_audio_task();
    assert(mode == SPDIF_MODE_USB_AUDIO);
    now_us += 2000001;
    buffered_frames = 0;
    underrun_frames = 1;
    usb_audio_task();
    assert(mode == SPDIF_MODE_SILENCE);
    now_us += UINT64_C(86400) * 1000000;
    usb_audio_task();
    assert(mode == SPDIF_MODE_SILENCE);
    buffered_frames = 1024;
    usb_audio_task();
    assert(mode == SPDIF_MODE_USB_AUDIO);

    supported = false;
    usb_audio_task();
    assert(mode == SPDIF_MODE_OFF);
#if PICOARC_UAC_VERSION == 2
    supported = true;
    select_alt(PICOARC_AUDIO_ALT_IEC61937_AC3);
    assert(format == SPDIF_STREAM_FORMAT_IEC61937);
    close_stream();
#endif
    puts("idle audio checks passed");
    return 0;
}
