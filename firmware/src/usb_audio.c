#include "usb_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arc.h"
#include "hardware/structs/usb.h"
#include "pico/time.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_descriptors.h"

enum {
    DEFAULT_SAMPLE_RATE = PICOARC_AUDIO_SAMPLE_RATE_LOW,
    MAX_SAMPLE_RATE = PICOARC_AUDIO_SAMPLE_RATE_HIGH,
    CHANNELS = 2,
    MAX_BYTES_PER_SAMPLE = 3,
    MAX_BYTES_PER_FRAME = CHANNELS * MAX_BYTES_PER_SAMPLE,
    MAX_FRAMES_PER_MS = MAX_SAMPLE_RATE / 1000,
    // Read up to ~2 ms of raw USB bytes per task pass; sized for the highest
    // supported rate so both 48 kHz and 96 kHz fit in the same buffers.
    READ_FRAMES_BUDGET = 2 * MAX_FRAMES_PER_MS,
    START_BUFFER_FRAMES = 256,
    RECOVER_BUFFER_FRAMES = 256,
    AUDIO_OUT_EP_NUM = 3,
    FEEDBACK_TARGET_FRAMES = 256,
    FEEDBACK_UPDATE_US = 4000,
    FEEDBACK_P_GAIN_Q16_PER_FRAME = 16,
    FEEDBACK_MAX_ADJUST_Q16 = 1 << 15,
};

#define HOST_VOLUME_MIN_DB (-20)
#define HOST_VOLUME_MAX_DB 0
#define HOST_VOLUME_STEP_DB 1
#define SOUNDBAR_VOLUME_MIN 0
#define SOUNDBAR_VOLUME_MAX 19

static int8_t mute[CHANNELS + 1];
static int16_t volume[CHANNELS + 1];
static uint8_t pcm_bytes[READ_FRAMES_BUDGET * MAX_BYTES_PER_FRAME];
static int32_t pcm_frames[READ_FRAMES_BUDGET * CHANNELS];
static bool streaming;
static uint8_t active_alt;
static uint32_t active_sample_rate = DEFAULT_SAMPLE_RATE;
static bool output_enabled;
static unsigned int refill_target_frames;
static unsigned int dropped_frames;
static uint64_t next_diag_log_us;
static uint64_t next_feedback_us;

static int16_t host_volume_min_256(void) {
    return HOST_VOLUME_MIN_DB * 256;
}

static int16_t host_volume_max_256(void) {
    return HOST_VOLUME_MAX_DB * 256;
}

static int16_t host_volume_step_256(void) {
    return HOST_VOLUME_STEP_DB * 256;
}

static int16_t clamp_host_volume(int16_t usb_volume) {
    const int16_t min_volume = host_volume_min_256();
    const int16_t max_volume = host_volume_max_256();

    if (usb_volume < min_volume) {
        return min_volume;
    }
    if (usb_volume > max_volume) {
        return max_volume;
    }
    return usb_volume;
}

static uint8_t clamp_soundbar_volume(uint8_t cec_volume) {
    if (cec_volume < SOUNDBAR_VOLUME_MIN) {
        return SOUNDBAR_VOLUME_MIN;
    }
    if (cec_volume > SOUNDBAR_VOLUME_MAX) {
        return SOUNDBAR_VOLUME_MAX;
    }
    return cec_volume;
}

static uint8_t usb_volume_to_cec_volume(int16_t usb_volume) {
    usb_volume = clamp_host_volume(usb_volume);

    const int32_t host_span = host_volume_max_256() - host_volume_min_256();
    const int32_t soundbar_span = SOUNDBAR_VOLUME_MAX - SOUNDBAR_VOLUME_MIN;
    if (host_span <= 0 || soundbar_span <= 0) {
        return SOUNDBAR_VOLUME_MIN;
    }

    return (uint8_t)(SOUNDBAR_VOLUME_MIN +
                     (((int32_t)(usb_volume - host_volume_min_256()) * soundbar_span + host_span / 2) / host_span));
}

static int16_t cec_volume_to_usb_volume(uint8_t cec_volume) {
    cec_volume = clamp_soundbar_volume(cec_volume);

    const int32_t host_span = host_volume_max_256() - host_volume_min_256();
    const int32_t soundbar_span = SOUNDBAR_VOLUME_MAX - SOUNDBAR_VOLUME_MIN;
    if (host_span <= 0 || soundbar_span <= 0) {
        return host_volume_min_256();
    }

    return (int16_t)(host_volume_min_256() +
                     (((int32_t)(cec_volume - SOUNDBAR_VOLUME_MIN) * host_span + soundbar_span / 2) / soundbar_span));
}

static void set_all_mute(int8_t value) {
    const int8_t normalized = value ? 1 : 0;

    for (unsigned int i = 0; i <= CHANNELS; i++) {
        mute[i] = normalized;
    }
}

static void set_all_volume(int16_t value) {
    for (unsigned int i = 0; i <= CHANNELS; i++) {
        volume[i] = value;
    }
}

static void notify_host_control_change(uint8_t control_selector) {
    const audio_interrupt_data_t data = {
        .bInfo = 0,
        .bAttribute = AUDIO_CS_REQ_CUR,
        .wValue_cn_or_mcn = 0,
        .wValue_cs = control_selector,
        .wIndex_ep_or_int = 0,
        .wIndex_entity_id = UAC2_ENTITY_FEATURE_UNIT,
    };

    tud_audio_int_write(&data);
}

void usb_audio_set_cec_audio_status(uint8_t cec_volume, bool muted) {
    const int8_t usb_mute = muted ? 1 : 0;
    const int16_t usb_volume = cec_volume_to_usb_volume(cec_volume);
    const bool mute_changed = mute[0] != usb_mute;
    const bool volume_changed = volume[0] != usb_volume;

    set_all_mute(usb_mute);
    set_all_volume(usb_volume);
    if (mute_changed) {
        notify_host_control_change(AUDIO_FU_CTRL_MUTE);
    }
    if (volume_changed) {
        notify_host_control_change(AUDIO_FU_CTRL_VOLUME);
    }
}

void usb_audio_set_cec_mute_status(bool muted) {
    const int8_t usb_mute = muted ? 1 : 0;
    const bool mute_changed = mute[0] != usb_mute;

    set_all_mute(usb_mute);
    if (mute_changed) {
        notify_host_control_change(AUDIO_FU_CTRL_MUTE);
    }
}

bool usb_audio_is_streaming(void) {
    return streaming;
}

static void update_feedback(bool force) {
    if (!streaming) {
        return;
    }

    const uint64_t now_us = time_us_64();
    if (!force && now_us < next_feedback_us) {
        return;
    }

    int32_t error = (int32_t)FEEDBACK_TARGET_FRAMES - (int32_t)spdif_buffered_frames();
    int32_t adjust_q16 = error * FEEDBACK_P_GAIN_Q16_PER_FRAME;
    if (adjust_q16 > FEEDBACK_MAX_ADJUST_Q16) {
        adjust_q16 = FEEDBACK_MAX_ADJUST_Q16;
    } else if (adjust_q16 < -FEEDBACK_MAX_ADJUST_Q16) {
        adjust_q16 = -FEEDBACK_MAX_ADJUST_Q16;
    }

    const uint32_t frames_per_ms = active_sample_rate / 1000;
    const uint32_t feedback_q16 = (frames_per_ms << 16) + (uint32_t)adjust_q16;
    tud_audio_n_fb_set(0, feedback_q16);
    next_feedback_us = now_us + FEEDBACK_UPDATE_US;
}

static bool clock_get_request(uint8_t rhport, audio_control_request_t const *request) {
    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(active_sample_rate)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
    }

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_RANGE) {
        // Two discrete rates: 48 kHz and 96 kHz. Most hosts treat each
        // sub-range whose bMin == bMax as a single supported value.
        audio_control_range_4_n_t(2) range = {
            .wNumSubRanges = tu_htole16(2),
            .subrange[0] = {
                .bMin = (int32_t)tu_htole32(PICOARC_AUDIO_SAMPLE_RATE_LOW),
                .bMax = (int32_t)tu_htole32(PICOARC_AUDIO_SAMPLE_RATE_LOW),
                .bRes = 0,
            },
            .subrange[1] = {
                .bMin = (int32_t)tu_htole32(PICOARC_AUDIO_SAMPLE_RATE_HIGH),
                .bMax = (int32_t)tu_htole32(PICOARC_AUDIO_SAMPLE_RATE_HIGH),
                .bRes = 0,
            },
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range, sizeof(range));
    }

    if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &valid, sizeof(valid));
    }

    return false;
}

static bool feature_get_request(uint8_t rhport, audio_control_request_t const *request) {
    if (request->bChannelNumber > CHANNELS) {
        return false;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur = {.bCur = mute[request->bChannelNumber]};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_2_t cur = {.bCur = tu_htole16(volume[request->bChannelNumber])};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME && request->bRequest == AUDIO_CS_REQ_RANGE) {
        audio_control_range_2_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {
                .bMin = tu_htole16(host_volume_min_256()),
                .bMax = tu_htole16(host_volume_max_256()),
                .bRes = tu_htole16(host_volume_step_256()),
            },
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range, sizeof(range));
    }

    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bEntityID == UAC2_ENTITY_CLOCK) {
        return clock_get_request(rhport, request);
    }

    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return feature_get_request(rhport, request);
    }

    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    (void)buf;
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;

    if (request->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }

    if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        const int32_t requested = ((audio_control_cur_4_t const *)buf)->bCur;
        if (requested != PICOARC_AUDIO_SAMPLE_RATE_LOW &&
            requested != PICOARC_AUDIO_SAMPLE_RATE_HIGH) {
            return false;
        }
        if ((uint32_t)requested != active_sample_rate) {
            active_sample_rate = (uint32_t)requested;
            spdif_set_sample_rate(active_sample_rate);
            printf("usb-audio: sample rate set to %lu Hz\n",
                   (unsigned long)active_sample_rate);
        }
        return true;
    }

    if (request->bEntityID != UAC2_ENTITY_FEATURE_UNIT) {
        return false;
    }

    if (request->bChannelNumber > CHANNELS) {
        return false;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        const int8_t requested_mute = ((audio_control_cur_1_t const *)buf)->bCur ? 1 : 0;
        set_all_mute(requested_mute);
        printf("usb-audio: host mute=%s ch=%u\n",
               requested_mute ? "on" : "off",
               request->bChannelNumber);
        arc_request_mute_sync(requested_mute != 0);
        return true;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        const int16_t requested_volume = ((audio_control_cur_2_t const *)buf)->bCur;
        set_all_volume(requested_volume);
        const uint8_t cec_volume = usb_volume_to_cec_volume(requested_volume);
        printf("usb-audio: host volume=%ld.%02lddB cec=%u ch=%u\n",
               (long)(requested_volume / 256),
               (long)(((requested_volume < 0 ? -requested_volume : requested_volume) % 256) * 100 / 256),
               cec_volume,
               request->bChannelNumber);
        arc_request_volume_sync(cec_volume);
        return true;
    }

    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING) {
        active_alt = alt;
        streaming = alt != PICOARC_AUDIO_ALT_ZERO;
        output_enabled = false;
        refill_target_frames = START_BUFFER_FRAMES;
        spdif_clear_usb_buffer();
        spdif_set_mode(SPDIF_MODE_SILENCE);
        dropped_frames = 0;
        next_diag_log_us = time_us_64() + 2000000;
        next_feedback_us = 0;
        update_feedback(true);
        const char *bit_depth =
            alt == PICOARC_AUDIO_ALT_PCM_24 ? "24" :
            alt == PICOARC_AUDIO_ALT_PCM_16 ? "16" : "off";
        printf("usb-audio: streaming %s (alt=%u, %lu Hz/%s-bit), spdif=%s\n",
               streaming ? "on" : "off", alt,
               (unsigned long)active_sample_rate, bit_depth,
               spdif_mode_name(spdif_get_mode()));
    }

    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    (void)request;
    streaming = false;
    active_alt = PICOARC_AUDIO_ALT_ZERO;
    output_enabled = false;
    refill_target_frames = START_BUFFER_FRAMES;
    spdif_clear_usb_buffer();
    spdif_set_mode(SPDIF_MODE_SILENCE);
    dropped_frames = 0;
    usb_dpram->ep_buf_ctrl[AUDIO_OUT_EP_NUM].out = 0;
    usb_dpram->ep_buf_ctrl[AUDIO_OUT_EP_NUM].in = 0;
    printf("usb-audio: streaming off, spdif=%s\n", spdif_mode_name(spdif_get_mode()));
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    feedback_param->sample_freq = active_sample_rate;
}

void usb_audio_task(void) {
    if (!streaming) {
        return;
    }

    update_feedback(false);

    const unsigned int bytes_per_sample = active_alt == PICOARC_AUDIO_ALT_PCM_24 ? 3u : 2u;
    const unsigned int bytes_per_frame = bytes_per_sample * CHANNELS;

    while (tud_audio_available() >= bytes_per_frame) {
        uint16_t available = tud_audio_available();
        uint16_t bytes = available;
        if (bytes > sizeof(pcm_bytes)) {
            bytes = sizeof(pcm_bytes);
        }
        bytes = (uint16_t)(bytes - (bytes % bytes_per_frame));
        if (bytes == 0) {
            return;
        }

        uint16_t read = tud_audio_read(pcm_bytes, bytes);
        unsigned int frames = read / bytes_per_frame;

        if (bytes_per_sample == 3u) {
            // Unpack interleaved 24-bit little-endian samples into 24-bit
            // left-aligned int32 (audio MSB at bit 31, audio LSB at bit 8).
            for (unsigned int i = 0; i < frames * CHANNELS; i++) {
                const uint8_t *p = &pcm_bytes[i * 3];
                const uint32_t raw = ((uint32_t)p[0] << 8) |
                                     ((uint32_t)p[1] << 16) |
                                     ((uint32_t)p[2] << 24);
                pcm_frames[i] = (int32_t)raw;
            }
        } else {
            // 16-bit input: promote into the same 24-bit-left-aligned int32.
            const int16_t *src = (const int16_t *)pcm_bytes;
            for (unsigned int i = 0; i < frames * CHANNELS; i++) {
                pcm_frames[i] = (int32_t)src[i] << 16;
            }
        }

        unsigned int written = spdif_write_pcm(pcm_frames, frames);
        if (written < frames) {
            dropped_frames += frames - written;
        }
    }

    if (!output_enabled && spdif_buffered_frames() >= refill_target_frames) {
        output_enabled = true;
        spdif_set_mode(SPDIF_MODE_USB_AUDIO);
        printf("usb-audio: output on buffered=%u\n", spdif_buffered_frames());
    }

    const uint64_t now_us = time_us_64();
    if (now_us >= next_diag_log_us) {
        spdif_usb_stats_t stats;
        spdif_take_usb_stats(&stats);
        printf("usb-audio: buf=%u hi=%u under=%u drop=%u\n",
               stats.buffered_frames, stats.high_water_frames, stats.underrun_frames, dropped_frames);
        if (output_enabled && stats.underrun_frames > 0) {
            output_enabled = false;
            refill_target_frames = RECOVER_BUFFER_FRAMES;
            spdif_set_mode(SPDIF_MODE_SILENCE);
            printf("usb-audio: output paused for refill buffered=%u\n", spdif_buffered_frames());
        }
        dropped_frames = 0;
        next_diag_log_us = now_us + 2000000;
    }

    update_feedback(false);
}
