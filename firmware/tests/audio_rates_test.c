#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "arc.h"
#include "spdif.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

static uint8_t caps[PICOARC_AUDIO_ALT_MAX + 1];
static uint8_t reply[64];
static uint16_t reply_len;
static uint32_t hardware_rate;

uint8_t arc_audio_rate_mask(uint8_t format) { return caps[format]; }
bool arc_audio_format_supported(uint8_t format, uint32_t rate) {
    return usb_audio_rate_in_mask(caps[format], rate);
}
bool arc_audio_format_supported_quiet(uint8_t format, uint32_t rate) {
    return arc_audio_format_supported(format, rate);
}
void arc_request_mute_sync(bool muted) { (void)muted; }
void arc_request_volume_sync(uint8_t volume) { (void)volume; }
void pico_get_unique_board_id_string(char *id, size_t len) { snprintf(id, len, "test"); }
uint64_t time_us_64(void) { return 1000; }
void spdif_set_sample_rate(uint32_t rate) { hardware_rate = rate; }
void spdif_clear_usb_buffer(void) {}
void spdif_set_mode(spdif_mode_t mode) { (void)mode; }
spdif_mode_t spdif_get_mode(void) { return SPDIF_MODE_OFF; }
void spdif_set_stream_format(spdif_stream_format_t format) { (void)format; }
unsigned int spdif_buffered_frames(void) { return 0; }
unsigned int spdif_write_pcm(const int32_t *samples, unsigned int count) { (void)samples; return count; }
#if PICOARC_UAC_VERSION == 2
void spdif_set_rate_adjustment_ppm(int32_t ppm) { (void)ppm; }
bool spdif_adaptive_buffered_frames(unsigned int *frames) { *frames = 0; return false; }
#endif
bool tud_audio_buffer_and_schedule_control_xfer(uint8_t port, tusb_control_request_t const *request,
                                               void *buffer, uint16_t length) {
    (void)port; (void)request;
    assert(length <= sizeof(reply));
    memcpy(reply, buffer, length);
    reply_len = length;
    return true;
}

const char *spdif_mode_name(spdif_mode_t mode) { (void)mode; return "off"; }
void spdif_take_usb_stats(spdif_usb_stats_t *stats) { memset(stats, 0, sizeof(*stats)); }
uint16_t tud_audio_n_available(uint8_t func) { (void)func; return 0; }
uint16_t tud_audio_n_read(uint8_t func, void *buffer, uint16_t size) {
    (void)func; (void)buffer; (void)size; return 0;
}
#if PICOARC_UAC_VERSION == 1
bool tud_audio_n_fb_set(uint8_t func, uint32_t feedback) { (void)func; (void)feedback; return true; }
#else
bool tud_audio_int_n_write(uint8_t func, const audio_interrupt_data_t *data) {
    (void)func; (void)data; return true;
}
#endif

static bool set_rate(uint32_t rate) {
    uint8_t data[] = {(uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24)};
    tusb_control_request_t req = {0};
#if PICOARC_UAC_VERSION == 1
    req.bRequest = AUDIO10_CS_REQ_SET_CUR;
    req.wValue = AUDIO10_EP_CTRL_SAMPLING_FREQ << 8;
    req.wIndex = PICOARC_AUDIO_EP_OUT;
    req.wLength = 3;
    return tud_audio_set_req_ep_cb(0, &req, data);
#else
    req.bRequest = AUDIO20_CS_REQ_CUR;
    req.wValue = AUDIO20_CS_CTRL_SAM_FREQ << 8;
    req.wIndex = (PICOARC_AUDIO_ENTITY_CLOCK << 8) | ITF_NUM_AUDIO_CONTROL;
    req.wLength = 4;
    return tud_audio_set_req_entity_cb(0, &req, data);
#endif
}

static uint32_t get_rate(void) {
    tusb_control_request_t req = {0};
#if PICOARC_UAC_VERSION == 1
    req.bRequest = AUDIO10_CS_REQ_GET_CUR;
    req.wValue = AUDIO10_EP_CTRL_SAMPLING_FREQ << 8;
    req.wIndex = PICOARC_AUDIO_EP_OUT;
    req.wLength = 3;
    assert(tud_audio_get_req_ep_cb(0, &req));
    assert(reply_len == 3);
    return reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16);
#else
    req.bRequest = AUDIO20_CS_REQ_CUR;
    req.wValue = AUDIO20_CS_CTRL_SAM_FREQ << 8;
    req.wIndex = (PICOARC_AUDIO_ENTITY_CLOCK << 8) | ITF_NUM_AUDIO_CONTROL;
    req.wLength = 4;
    assert(tud_audio_get_req_entity_cb(0, &req));
    assert(reply_len == 4);
    return tu_unaligned_read32(reply);
#endif
}

static bool select_alt(uint8_t alt) {
    tusb_control_request_t req = {.wIndex = ITF_NUM_AUDIO_STREAMING, .wValue = alt};
    return tud_audio_set_itf_cb(0, &req);
}

static void check_descriptors(unsigned int expected_alts) {
    const uint8_t *desc = tud_descriptor_configuration_cb(0);
    const unsigned int total = desc[2] | ((unsigned int)desc[3] << 8);
    unsigned int count = 0, formats = 0;
    unsigned int offset = 0;
    uint8_t alt = 0;
    bool streaming = false;
    while (offset < total) {
        const uint8_t *d = desc + offset;
        assert(d[0] >= 2 && offset + d[0] <= total);
        if (d[1] == TUSB_DESC_INTERFACE) {
            streaming = d[2] == ITF_NUM_AUDIO_STREAMING;
            alt = streaming ? d[3] : 0;
            if (alt) {
                assert(alt == ++count);
                assert(caps[usb_descriptors_audio_format(alt)] != 0);
            }
        }
        if (streaming && alt && d[1] == TUSB_DESC_CS_INTERFACE && d[2] == 2) {
            ++formats;
#if PICOARC_UAC_VERSION == 1
            assert(d[0] == 8 + 3 * d[7]);
            assert(d[7] != 0);
            uint8_t advertised = 0;
            for (unsigned int i = 0; i < d[7]; ++i) {
                const uint8_t *r = d + 8 + 3 * i;
                uint32_t rate = r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16);
                assert(usb_audio_rate_in_mask(caps[usb_descriptors_audio_format(alt)], rate));
                for (unsigned int bit = 0; bit < 5; ++bit) {
                    if (rate == usb_audio_rate_at(bit)) advertised |= 1u << bit;
                }
            }
            assert(advertised == caps[usb_descriptors_audio_format(alt)]);
#endif
        }
        offset += d[0];
    }
    assert(offset == total && count == expected_alts && formats == expected_alts);
#if PICOARC_UAC_VERSION == 2
    tusb_control_request_t req = {
        .bRequest = AUDIO20_CS_REQ_RANGE, .wValue = AUDIO20_CS_CTRL_SAM_FREQ << 8,
        .wIndex = (PICOARC_AUDIO_ENTITY_CLOCK << 8) | ITF_NUM_AUDIO_CONTROL,
    };
    assert(tud_audio_get_req_entity_cb(0, &req));
    uint8_t expected_mask = 0;
    for (unsigned int i = 1; i <= PICOARC_AUDIO_ALT_MAX; ++i) expected_mask |= caps[i];
    unsigned int ranges = reply[0] | ((unsigned int)reply[1] << 8);
    assert(reply_len == 2 + 12 * ranges);
    unsigned int index = 0;
    for (unsigned int bit = 0; bit < 5; ++bit) {
        if (!(expected_mask & (1u << bit))) continue;
        assert(index < ranges);
        const uint8_t *r = reply + 2 + 12 * index++;
        assert(tu_unaligned_read32(r) == usb_audio_rate_at(bit));
        assert(tu_unaligned_read32(r + 4) == usb_audio_rate_at(bit));
        assert(tu_unaligned_read32(r + 8) == 0);
    }
    assert(index == ranges);
#endif
}

int main(void) {
    // No sink/capabilities: never advertise or accept a speculative rate.
    usb_audio_prepare_connection();
    check_descriptors(0);
    assert(!set_rate(48000));
    assert(!select_alt(1));

    // Bose's captured SAD: stereo PCM at 32/44.1/48 kHz, all three bit depths.
    caps[1] = caps[2] = caps[3] = 7;
    assert(usb_descriptors_audio_changed());
    usb_audio_prepare_connection();
    assert(!usb_descriptors_audio_changed());
    check_descriptors(3);
    assert(get_rate() == 48000);
    assert(select_alt(1));
#if PICOARC_UAC_VERSION == 1
    assert(set_rate(96000)); // Required discrete-frequency rounding, never 96 kHz.
    assert(get_rate() == 48000);
#else
    assert(!set_rate(96000));
    assert(get_rate() == 48000);
#endif
    assert(set_rate(44100));
    assert(get_rate() == 44100 && hardware_rate == 44100);
    assert(!select_alt(PICOARC_AUDIO_ALT_MAX + 1));
    assert(select_alt(0));

    // Swapping to a 96-kHz sink expands the list; swapping back contracts it.
    caps[1] = caps[2] = caps[3] = 31;
    assert(usb_descriptors_audio_changed());
    usb_audio_prepare_connection();
    check_descriptors(3);
    assert(set_rate(96000));
    assert(get_rate() == 96000 && hardware_rate == 96000);
    caps[1] = caps[2] = caps[3] = 7;
    assert(usb_descriptors_audio_changed());
    usb_audio_prepare_connection();
    check_descriptors(3);
    assert(get_rate() == 48000);

    // Rates may differ by bit depth: do not accept a rate belonging only to a
    // different format while this one is streaming.
    caps[1] = 31; caps[2] = 7; caps[3] = 7;
    usb_audio_prepare_connection();
    check_descriptors(3);
    assert(select_alt(3));
#if PICOARC_UAC_VERSION == 2
    assert(!set_rate(96000));
#else
    assert(set_rate(96000));
#endif
    assert(get_rate() == 48000);
    assert(select_alt(0));
    assert(set_rate(96000));
#if PICOARC_UAC_VERSION == 2
    assert(!select_alt(3));
#endif
    assert(select_alt(1));
    assert(get_rate() == 96000);
    assert(select_alt(0));

    // Missing bit depths compact the USB alt numbers without changing formats.
    caps[1] = 0; caps[2] = 0; caps[3] = 2;
    usb_audio_prepare_connection();
    check_descriptors(1);
    assert(usb_descriptors_audio_format(1) == PICOARC_AUDIO_ALT_PCM_24);
    assert(get_rate() == 44100);
    assert(select_alt(1));
    assert(select_alt(0));

    // Existing no-SAD fallback remains exactly 16-bit PCM at 48 kHz.
    caps[1] = 4; caps[2] = 0; caps[3] = 0;
    usb_audio_prepare_connection();
    check_descriptors(1);
    assert(get_rate() == 48000);
#if PICOARC_UAC_VERSION == 2
    caps[PICOARC_AUDIO_ALT_IEC61937_DTS] = 4;
    usb_audio_prepare_connection();
    check_descriptors(2);
    assert(usb_descriptors_audio_format(2) == PICOARC_AUDIO_ALT_IEC61937_DTS);
    assert(select_alt(2));
    assert(select_alt(0));
#endif
    // Reject stale host requests immediately after sink capability loss.
    memset(caps, 0, sizeof(caps));
    assert(!set_rate(48000));
    assert(!select_alt(1));
    usb_audio_prepare_connection();
    check_descriptors(0);
    puts("sample-rate negotiation checks passed");
    return 0;
}
