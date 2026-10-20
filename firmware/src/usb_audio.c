#include "usb_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arc.h"
#include "audio_samples.h"
#include "picoarc_config.h"
#include "picoarc_log.h"
#include "pico/time.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_descriptors.h"
#if PICOARC_USB_DIAGNOSTICS
#include "ddc_edid.h"
#include "diagnostics_trace.h"
#include "hardware/gpio.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/watchdog.h"
#include "pico/bootrom.h"
#endif

enum {
    DEFAULT_SAMPLE_RATE = PICOARC_AUDIO_SAMPLE_RATE_DEFAULT,
    MAX_SAMPLE_RATE = PICOARC_AUDIO_SAMPLE_RATE_MAX,
    CHANNELS = 2,
    MAX_BYTES_PER_SAMPLE = 3,
    MAX_BYTES_PER_FRAME = CHANNELS * MAX_BYTES_PER_SAMPLE,
    MAX_FRAMES_PER_MS = MAX_SAMPLE_RATE / 1000,
    // Read up to ~2 ms of raw USB bytes per task pass; sized for the highest
    // advertised rate so all supported rates fit in the same buffers.
    READ_FRAMES_BUDGET = 2 * MAX_FRAMES_PER_MS,
#if PICOARC_UAC_VERSION == 1
    START_BUFFER_FRAMES = 256,
    RECOVER_BUFFER_FRAMES = 256,
    FEEDBACK_TARGET_FRAMES = 256,
    FEEDBACK_UPDATE_US = 2000,
    FEEDBACK_P_GAIN_Q16_PER_FRAME = 16,
    FEEDBACK_MAX_ADJUST_Q16 = 1 << 15,
#else
    // Core 1 stages audio in 192-frame S/PDIF blocks. Preload one full block
    // beyond the post-prefetch control target before enabling live output.
    START_BUFFER_FRAMES = 512,
    RECOVER_BUFFER_FRAMES = 512,
    ADAPTIVE_CLOCK_TARGET_FRAMES = 256,
    ADAPTIVE_CLOCK_UPDATE_US = 20000,
    ADAPTIVE_CLOCK_P_GAIN_PPM_PER_FRAME = 4,
    ADAPTIVE_CLOCK_I_DIVISOR = 128,
    ADAPTIVE_CLOCK_MAX_ADJUST_PPM = 1000,
#endif
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
static unsigned int gated_frames;
static uint64_t next_diag_log_us;
#if PICOARC_UAC_VERSION == 1
static uint64_t next_feedback_us;
#else
static uint64_t next_adaptive_clock_us;
static int32_t adaptive_clock_integral_error;
static int32_t adaptive_clock_adjust_ppm;
#endif
static bool audio_gate_open;
static uint64_t next_gate_log_us;
static uint64_t last_audio_task_us;
static uint64_t max_audio_task_gap_us;
static uint64_t last_audio_read_us;
static uint64_t max_audio_read_gap_us;
static uint16_t max_usb_available_bytes;
static uint64_t stream_packet_deadline_us;
static volatile uint32_t stream_packet_count;
static uint32_t observed_stream_packet_count;
static bool recovery_requested;
#if PICOARC_USB_DIAGNOSTICS
static volatile uint32_t diagnostics_rx_packets;
static volatile uint32_t diagnostics_rx_bytes;
static uint32_t diagnostics_nonzero_bytes;
static uint32_t diagnostics_written_frames;
static uint32_t diagnostics_gated_frames;
static bool diagnostics_usb_reconnect_pending;
static uint32_t diagnostics_usb_reconnect_requests;
static uint32_t diagnostics_usb_reconnect_uptime_ms;
static bool diagnostics_hdmi_recovery_pending;
static uint32_t diagnostics_hdmi_recovery_requests;
static uint32_t diagnostics_hdmi_recovery_uptime_ms;
static uint8_t diagnostics_isolated_pending;
static uint32_t diagnostics_isolated_requests;
static uint32_t diagnostics_isolated_uptime_ms;
static uint32_t diagnostics_last_isolated_action;
#endif
#if PICOARC_UAC_VERSION == 2
static uint8_t control_notifications_pending;
#endif

static bool active_alt_is_iec61937(void) {
#if PICOARC_UAC_VERSION == 2
    return active_alt == PICOARC_AUDIO_ALT_IEC61937_AC3 ||
           active_alt == PICOARC_AUDIO_ALT_IEC61937_DTS;
#else
    return false;
#endif
}

static spdif_mode_t idle_spdif_mode(void) {
#if PICOARC_IDLE_AUDIO_KEEPALIVE
    return SPDIF_MODE_SILENCE;
#else
    return SPDIF_MODE_OFF;
#endif
}

static bool active_alt_uses_24_bit_subslot(void) {
    return active_alt == PICOARC_AUDIO_ALT_PCM_20 ||
           active_alt == PICOARC_AUDIO_ALT_PCM_24;
}

static const char *active_alt_format_name(void) {
    switch (active_alt) {
    case PICOARC_AUDIO_ALT_PCM_16:
        return "PCM 16-bit";
    case PICOARC_AUDIO_ALT_PCM_20:
        return "PCM 20-bit";
    case PICOARC_AUDIO_ALT_PCM_24:
        return "PCM 24-bit";
#if PICOARC_UAC_VERSION == 2
    case PICOARC_AUDIO_ALT_IEC61937_AC3:
        return "IEC 61937 AC-3";
    case PICOARC_AUDIO_ALT_IEC61937_DTS:
        return "IEC 61937 DTS";
#endif
    default:
        return "off";
    }
}

static uint8_t available_rate_mask(uint8_t format) {
    uint8_t live = 0;
    if (format == 0) {
        for (unsigned int i = 1; i <= PICOARC_AUDIO_ALT_MAX; ++i) {
            live |= arc_audio_rate_mask(i);
        }
    } else {
        live = arc_audio_rate_mask(format);
    }
    return usb_descriptors_audio_rate_mask(format) & live;
}

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

static bool set_all_mute(int8_t value) {
    const int8_t normalized = value ? 1 : 0;
    bool changed = false;

    for (unsigned int i = 0; i <= CHANNELS; i++) {
        changed = changed || mute[i] != normalized;
        mute[i] = normalized;
    }

    return changed;
}

static bool set_all_volume(int16_t value) {
    bool changed = false;

    for (unsigned int i = 0; i <= CHANNELS; i++) {
        changed = changed || volume[i] != value;
        volume[i] = value;
    }

    return changed;
}

#if PICOARC_UAC_VERSION == 2
enum {
    CONTROL_NOTIFICATION_MUTE = 1u << 0,
    CONTROL_NOTIFICATION_VOLUME = 1u << 1,
};

static void queue_host_control_change(uint8_t control_selector) {
    if (control_selector == AUDIO20_FU_CTRL_MUTE) {
        control_notifications_pending |= CONTROL_NOTIFICATION_MUTE;
    } else if (control_selector == AUDIO20_FU_CTRL_VOLUME) {
        control_notifications_pending |= CONTROL_NOTIFICATION_VOLUME;
    }
}

static void update_control_notifications(void) {
    uint8_t pending_bit;
    uint8_t control_selector;

    if (control_notifications_pending & CONTROL_NOTIFICATION_MUTE) {
        pending_bit = CONTROL_NOTIFICATION_MUTE;
        control_selector = AUDIO20_FU_CTRL_MUTE;
    } else if (control_notifications_pending & CONTROL_NOTIFICATION_VOLUME) {
        pending_bit = CONTROL_NOTIFICATION_VOLUME;
        control_selector = AUDIO20_FU_CTRL_VOLUME;
    } else {
        return;
    }

    const audio_interrupt_data_t data = {
        .v2 = {
            .bInfo = 0,
            .bAttribute = AUDIO20_CS_REQ_CUR,
            .wValue_cn_or_mcn = 0,
            .wValue_cs = control_selector,
            .wIndex_ep_or_int = ITF_NUM_AUDIO_CONTROL,
            .wIndex_entity_id = PICOARC_AUDIO_ENTITY_FEATURE_UNIT,
        },
    };

    if (tud_audio_int_write(&data)) {
        control_notifications_pending &= (uint8_t)~pending_bit;
    }
}
#endif

void usb_audio_set_cec_audio_status(uint8_t cec_volume, bool muted, bool notify_host) {
    const int8_t usb_mute = muted ? 1 : 0;
    const int16_t usb_volume = cec_volume_to_usb_volume(cec_volume);
    const bool mute_changed = set_all_mute(usb_mute);
    const bool volume_changed = set_all_volume(usb_volume);

#if PICOARC_UAC_VERSION == 2
    if (notify_host && mute_changed) {
        queue_host_control_change(AUDIO20_FU_CTRL_MUTE);
    }
    if (notify_host && volume_changed) {
        queue_host_control_change(AUDIO20_FU_CTRL_VOLUME);
    }
#else
    (void)notify_host;
    (void)mute_changed;
    (void)volume_changed;
#endif
}

void usb_audio_set_cec_mute_status(bool muted, bool notify_host) {
    const int8_t usb_mute = muted ? 1 : 0;
    const bool changed = set_all_mute(usb_mute);

#if PICOARC_UAC_VERSION == 2
    if (notify_host && changed) {
        queue_host_control_change(AUDIO20_FU_CTRL_MUTE);
    }
#else
    (void)notify_host;
    (void)changed;
#endif
}

bool usb_audio_is_streaming(void) {
    return streaming;
}

bool usb_audio_take_recovery_request(void) {
    bool requested = recovery_requested;
    recovery_requested = false;
#if PICOARC_USB_DIAGNOSTICS
    requested |= diagnostics_usb_reconnect_pending;
    diagnostics_usb_reconnect_pending = false;
#endif
    return requested;
}

#if PICOARC_UAC_VERSION == 1
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

    const uint32_t nominal_q16 = (uint32_t)(((uint64_t)active_sample_rate << 16) / 1000u);
    const uint32_t feedback_q16 = nominal_q16 + (uint32_t)adjust_q16;
    tud_audio_n_fb_set(0, feedback_q16);
    next_feedback_us = now_us + FEEDBACK_UPDATE_US;
}
#else
static void reset_adaptive_clock(void) {
    next_adaptive_clock_us = 0;
    adaptive_clock_integral_error = 0;
    adaptive_clock_adjust_ppm = 0;
    spdif_set_rate_adjustment_ppm(0);
}

static void update_adaptive_clock(bool force) {
    if (!streaming || !output_enabled || !audio_gate_open) {
        return;
    }

    const uint64_t now_us = time_us_64();
    if (!force && now_us < next_adaptive_clock_us) {
        return;
    }

    unsigned int buffered_frames;
    if (!spdif_adaptive_buffered_frames(&buffered_frames)) {
        return;
    }

    const int32_t error = (int32_t)buffered_frames -
                          ADAPTIVE_CLOCK_TARGET_FRAMES;
    const int32_t integral_limit = ADAPTIVE_CLOCK_MAX_ADJUST_PPM *
                                   ADAPTIVE_CLOCK_I_DIVISOR;
    int32_t candidate_integral = adaptive_clock_integral_error + error;
    if (candidate_integral > integral_limit) {
        candidate_integral = integral_limit;
    } else if (candidate_integral < -integral_limit) {
        candidate_integral = -integral_limit;
    }

    const int32_t proportional_ppm = error *
                                     ADAPTIVE_CLOCK_P_GAIN_PPM_PER_FRAME;
    int32_t adjust_ppm = proportional_ppm +
                         candidate_integral / ADAPTIVE_CLOCK_I_DIVISOR;
    // Do not wind the integral farther into a saturated output. It may still
    // unwind immediately when the buffer error changes direction.
    if ((adjust_ppm > ADAPTIVE_CLOCK_MAX_ADJUST_PPM && error > 0) ||
        (adjust_ppm < -ADAPTIVE_CLOCK_MAX_ADJUST_PPM && error < 0)) {
        adjust_ppm = proportional_ppm +
                     adaptive_clock_integral_error /
                         ADAPTIVE_CLOCK_I_DIVISOR;
    } else {
        adaptive_clock_integral_error = candidate_integral;
    }

    if (adjust_ppm > ADAPTIVE_CLOCK_MAX_ADJUST_PPM) {
        adjust_ppm = ADAPTIVE_CLOCK_MAX_ADJUST_PPM;
    } else if (adjust_ppm < -ADAPTIVE_CLOCK_MAX_ADJUST_PPM) {
        adjust_ppm = -ADAPTIVE_CLOCK_MAX_ADJUST_PPM;
    }

    adaptive_clock_adjust_ppm = adjust_ppm;
    spdif_set_rate_adjustment_ppm(adjust_ppm);
    next_adaptive_clock_us = now_us + ADAPTIVE_CLOCK_UPDATE_US;
}
#endif

static void apply_sample_rate(uint32_t sample_rate) {
    if (sample_rate == active_sample_rate) {
        return;
    }

    active_sample_rate = sample_rate;
    spdif_set_sample_rate(active_sample_rate);
    output_enabled = false;
    audio_gate_open = false;
    refill_target_frames = START_BUFFER_FRAMES;
    spdif_clear_usb_buffer();
    spdif_set_mode(idle_spdif_mode());
#if PICOARC_UAC_VERSION == 1
    next_feedback_us = 0;
    update_feedback(true);
#else
    reset_adaptive_clock();
#endif
    printf("usb-audio: sample rate set to %lu Hz\n",
           (unsigned long)active_sample_rate);
}

void usb_audio_prepare_connection(void) {
    usb_descriptors_prepare_audio();
    const uint32_t rate = usb_audio_nearest_rate(usb_descriptors_audio_rate_mask(0),
                                               PICOARC_AUDIO_SAMPLE_RATE_DEFAULT);
    if (rate != 0) {
        apply_sample_rate(rate);
    }
}

#if PICOARC_UAC_VERSION == 1

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t endpoint = TU_U16_LOW(request->wIndex);
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);

    if (endpoint != PICOARC_AUDIO_EP_OUT ||
        control_selector != AUDIO10_EP_CTRL_SAMPLING_FREQ ||
        request->bRequest != AUDIO10_CS_REQ_GET_CUR ||
        request->wLength != 3) {
        return false;
    }

    uint8_t frequency[3] = {
        (uint8_t)(active_sample_rate & 0xffu),
        (uint8_t)((active_sample_rate >> 8) & 0xffu),
        (uint8_t)((active_sample_rate >> 16) & 0xffu),
    };
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                       frequency,
                                                       sizeof(frequency));
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf) {
    (void)rhport;
    const uint8_t endpoint = TU_U16_LOW(request->wIndex);
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);

    if (endpoint != PICOARC_AUDIO_EP_OUT ||
        control_selector != AUDIO10_EP_CTRL_SAMPLING_FREQ ||
        request->bRequest != AUDIO10_CS_REQ_SET_CUR ||
        request->wLength != 3) {
        return false;
    }

    const uint32_t requested = (uint32_t)buf[0] |
                               ((uint32_t)buf[1] << 8) |
                               ((uint32_t)buf[2] << 16);
    // UAC1 requires rounding unsupported discrete-frequency requests.
    const uint32_t selected = usb_audio_nearest_rate(available_rate_mask(active_alt), requested);
    if (selected == 0) {
        return false;
    }
    apply_sample_rate(selected);
    return true;
}

static bool feature_get_request(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel_number = TU_U16_LOW(request->wValue);

    if (channel_number != 0) {
        return false;
    }

    if (control_selector == AUDIO10_FU_CTRL_MUTE &&
        request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        uint8_t current = (uint8_t)mute[channel_number];
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &current,
                                                           sizeof(current));
    }

    if (control_selector == AUDIO10_FU_CTRL_VOLUME) {
        int16_t value;

        switch (request->bRequest) {
        case AUDIO10_CS_REQ_GET_CUR:
            value = volume[channel_number];
            break;
        case AUDIO10_CS_REQ_GET_MIN:
            value = host_volume_min_256();
            break;
        case AUDIO10_CS_REQ_GET_MAX:
            value = host_volume_max_256();
            break;
        case AUDIO10_CS_REQ_GET_RES:
            value = host_volume_step_256();
            break;
        default:
            return false;
        }

        uint16_t response = tu_htole16((uint16_t)value);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &response,
                                                           sizeof(response));
    }

    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);

    if (entity_id == PICOARC_AUDIO_ENTITY_FEATURE_UNIT) {
        return feature_get_request(rhport, request);
    }

    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf) {
    (void)rhport;
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel_number = TU_U16_LOW(request->wValue);

    if (request->bRequest != AUDIO10_CS_REQ_SET_CUR ||
        entity_id != PICOARC_AUDIO_ENTITY_FEATURE_UNIT ||
        channel_number != 0) {
        return false;
    }

    if (control_selector == AUDIO10_FU_CTRL_MUTE) {
        if (request->wLength != 1) {
            return false;
        }
        const int8_t requested_mute = buf[0] ? 1 : 0;
        set_all_mute(requested_mute);
        if (!streaming) {
            printf("usb-audio: host mute=%s ch=%u\n",
                   requested_mute ? "on" : "off",
                   channel_number);
        }
        arc_request_mute_sync(requested_mute != 0);
        return true;
    }

    if (control_selector == AUDIO10_FU_CTRL_VOLUME) {
        if (request->wLength != 2) {
            return false;
        }
        const int16_t requested_volume = clamp_host_volume(
            (int16_t)tu_le16toh(tu_unaligned_read16(buf)));
        set_all_volume(requested_volume);
        const uint8_t cec_volume = usb_volume_to_cec_volume(requested_volume);
        if (!streaming) {
            printf("usb-audio: host volume=%ld.%02lddB cec=%u ch=%u\n",
                   (long)(requested_volume / 256),
                   (long)(((requested_volume < 0 ? -requested_volume : requested_volume) % 256) * 100 / 256),
                   cec_volume,
                   channel_number);
        }
        arc_request_volume_sync(cec_volume);
        return true;
    }

    return false;
}

#else

_Static_assert(sizeof(audio20_control_range_4_n_t(PICOARC_AUDIO_SAMPLE_RATE_COUNT)) <=
                   CFG_TUD_AUDIO_CTRL_BUF_SZ,
               "UAC2 sample-rate range exceeds TinyUSB control buffer");

static bool clock_get_request(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);

    if (control_selector == AUDIO20_CS_CTRL_SAM_FREQ &&
        request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_4_t cur = {
            .bCur = (int32_t)tu_htole32(active_sample_rate),
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &cur, sizeof(cur));
    }

    if (control_selector == AUDIO20_CS_CTRL_SAM_FREQ &&
        request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(PICOARC_AUDIO_SAMPLE_RATE_COUNT) range = {0};
        const uint8_t mask = usb_descriptors_audio_rate_mask(0);
        unsigned int count = 0;
        for (unsigned int i = 0; i < PICOARC_AUDIO_SAMPLE_RATE_COUNT; ++i) {
            const uint32_t rate = usb_audio_rate_at(i);
            if (usb_audio_rate_in_mask(mask, rate)) {
                range.subrange[count].bMin = (int32_t)tu_htole32(rate);
                range.subrange[count].bMax = (int32_t)tu_htole32(rate);
                ++count;
            }
        }
        range.wNumSubRanges = tu_htole16(count);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &range, 2 + 12 * count);
    }

    if (control_selector == AUDIO20_CS_CTRL_CLK_VALID &&
        request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = {
            .bCur = usb_audio_rate_in_mask(available_rate_mask(active_alt), active_sample_rate),
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &valid, sizeof(valid));
    }

    return false;
}

static bool feature_get_request(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel_number = TU_U16_LOW(request->wValue);

    if (channel_number != 0) {
        return false;
    }

    if (control_selector == AUDIO20_FU_CTRL_MUTE &&
        request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t cur = {.bCur = mute[channel_number]};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &cur, sizeof(cur));
    }

    if (control_selector == AUDIO20_FU_CTRL_VOLUME &&
        request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_2_t cur = {
            .bCur = (int16_t)tu_htole16((uint16_t)volume[channel_number]),
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &cur, sizeof(cur));
    }

    if (control_selector == AUDIO20_FU_CTRL_VOLUME &&
        request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {
                .bMin = (int16_t)tu_htole16((uint16_t)host_volume_min_256()),
                .bMax = (int16_t)tu_htole16((uint16_t)host_volume_max_256()),
                .bRes = tu_htole16((uint16_t)host_volume_step_256()),
            },
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                           &range, sizeof(range));
    }

    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request) {
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);

    if (entity_id == PICOARC_AUDIO_ENTITY_CLOCK) {
        return clock_get_request(rhport, request);
    }
    if (entity_id == PICOARC_AUDIO_ENTITY_FEATURE_UNIT) {
        return feature_get_request(rhport, request);
    }

    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf) {
    (void)rhport;
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);
    const uint8_t control_selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel_number = TU_U16_LOW(request->wValue);

    if (request->bRequest != AUDIO20_CS_REQ_CUR) {
        return false;
    }

    if (entity_id == PICOARC_AUDIO_ENTITY_CLOCK &&
        control_selector == AUDIO20_CS_CTRL_SAM_FREQ) {
        if (request->wLength != sizeof(audio20_control_cur_4_t)) {
            return false;
        }
        const uint32_t requested = tu_le32toh(tu_unaligned_read32(buf));
        if (!usb_audio_rate_in_mask(available_rate_mask(active_alt), requested)) {
            return false;
        }
        apply_sample_rate(requested);
        return true;
    }

    if (entity_id != PICOARC_AUDIO_ENTITY_FEATURE_UNIT ||
        channel_number != 0) {
        return false;
    }

    if (control_selector == AUDIO20_FU_CTRL_MUTE) {
        if (request->wLength != sizeof(audio20_control_cur_1_t)) {
            return false;
        }
        const int8_t requested_mute = buf[0] ? 1 : 0;
        set_all_mute(requested_mute);
        if (!streaming) {
            printf("usb-audio: host mute=%s ch=%u\n",
                   requested_mute ? "on" : "off",
                   channel_number);
        }
        arc_request_mute_sync(requested_mute != 0);
        return true;
    }

    if (control_selector == AUDIO20_FU_CTRL_VOLUME) {
        if (request->wLength != sizeof(audio20_control_cur_2_t)) {
            return false;
        }
        const int16_t requested_volume = clamp_host_volume(
            (int16_t)tu_le16toh(tu_unaligned_read16(buf)));
        set_all_volume(requested_volume);
        const uint8_t cec_volume = usb_volume_to_cec_volume(requested_volume);
        if (!streaming) {
            printf("usb-audio: host volume=%ld.%02lddB cec=%u ch=%u\n",
                   (long)(requested_volume / 256),
                   (long)(((requested_volume < 0 ? -requested_volume : requested_volume) % 256) * 100 / 256),
                   cec_volume,
                   channel_number);
        }
        arc_request_volume_sync(cec_volume);
        return true;
    }

    return false;
}

#endif

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING) {
        const uint8_t format = usb_descriptors_audio_format(alt);
        if (alt != 0 && (format == 0 || available_rate_mask(format) == 0)) {
            return false;
        }
#if PICOARC_UAC_VERSION == 2
        if (alt != 0 && !usb_audio_rate_in_mask(available_rate_mask(format), active_sample_rate)) {
            return false;
        }
#else
        // UAC1 hosts select the interface before setting its endpoint rate.
        if (alt != 0) {
            apply_sample_rate(usb_audio_nearest_rate(available_rate_mask(format), active_sample_rate));
        }
#endif

        const uint64_t now_us = time_us_64();
        active_alt = format;
#if PICOARC_USB_DIAGNOSTICS
        diagnostics_trace_record((uint32_t)(now_us / 1000u), DIAGNOSTICS_TRACE_USB_ALT,
                                  0, alt, NULL, 0);
#endif
        streaming = alt != PICOARC_AUDIO_ALT_ZERO;
        stream_packet_count = 0;
        observed_stream_packet_count = 0;
        // Arm recovery only after the first packet. Windows may select an
        // active alt before playback starts and legitimately send no data.
        stream_packet_deadline_us = 0;
        recovery_requested = false;
        const bool initial_gate_open = streaming &&
                                       arc_audio_format_supported_quiet(active_alt, active_sample_rate);
        output_enabled = false;
        audio_gate_open = false;
        refill_target_frames = START_BUFFER_FRAMES;
        spdif_clear_usb_buffer();
        spdif_set_stream_format(active_alt_is_iec61937() ?
                                SPDIF_STREAM_FORMAT_IEC61937 :
                                SPDIF_STREAM_FORMAT_PCM);
        spdif_set_mode(initial_gate_open ? SPDIF_MODE_SILENCE : idle_spdif_mode());
        dropped_frames = 0;
        gated_frames = 0;
        next_diag_log_us = now_us + 2000000;
        next_gate_log_us = 0;
#if PICOARC_UAC_VERSION == 1
        next_feedback_us = 0;
#else
        reset_adaptive_clock();
#endif
        last_audio_task_us = 0;
        max_audio_task_gap_us = 0;
        last_audio_read_us = 0;
        max_audio_read_gap_us = 0;
        max_usb_available_bytes = 0;
#if PICOARC_UAC_VERSION == 1
        update_feedback(true);
#endif
        printf("usb-audio: streaming %s (alt=%u, %lu Hz, %s), spdif=%s gate=%s\n",
               streaming ? "on" : "off", alt,
               (unsigned long)active_sample_rate, active_alt_format_name(),
               spdif_mode_name(spdif_get_mode()),
               initial_gate_open ? "open" : "closed");
    }

    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    (void)request;
    usb_audio_stop_streaming();
    printf("usb-audio: streaming off, spdif=%s\n", spdif_mode_name(spdif_get_mode()));
    return true;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received,
                           uint8_t func_id, uint8_t ep_out,
                           uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

#if PICOARC_USB_DIAGNOSTICS
    diagnostics_rx_packets++;
    diagnostics_rx_bytes += n_bytes_received;
#endif
    if (n_bytes_received > 0) {
        stream_packet_count++;
    }
    return true;
}

void usb_audio_stop_streaming(void) {
    streaming = false;
    active_alt = PICOARC_AUDIO_ALT_ZERO;
    output_enabled = false;
    refill_target_frames = START_BUFFER_FRAMES;
    spdif_clear_usb_buffer();
    spdif_set_stream_format(SPDIF_STREAM_FORMAT_PCM);
    spdif_set_mode(idle_spdif_mode());
    dropped_frames = 0;
    gated_frames = 0;
    audio_gate_open = false;
    next_gate_log_us = 0;
    last_audio_task_us = 0;
    max_audio_task_gap_us = 0;
    last_audio_read_us = 0;
    max_audio_read_gap_us = 0;
    max_usb_available_bytes = 0;
    stream_packet_deadline_us = 0;
    stream_packet_count = 0;
    observed_stream_packet_count = 0;
    recovery_requested = false;
#if PICOARC_UAC_VERSION == 2
    reset_adaptive_clock();
#endif
}

#if PICOARC_UAC_VERSION == 1
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    feedback_param->sample_freq = active_sample_rate;
}
#endif

void usb_audio_task(void) {
#if PICOARC_UAC_VERSION == 2
    update_control_notifications();
#endif

    if (!streaming) {
        return;
    }

    const uint64_t task_now_us = time_us_64();
    const uint32_t packet_count = stream_packet_count;
    if (packet_count != observed_stream_packet_count) {
        observed_stream_packet_count = packet_count;
        stream_packet_deadline_us =
            task_now_us + (uint64_t)PICOARC_USB_STREAM_PACKET_TIMEOUT_MS * 1000u;
    } else if (stream_packet_deadline_us != 0 && task_now_us >= stream_packet_deadline_us) {
        recovery_requested = true;
        stream_packet_deadline_us = 0;
        printf("usb-audio: packets stalled at alt=%u; requesting USB recovery\n",
               active_alt);
        return;
    }

    if (last_audio_task_us != 0) {
        const uint64_t gap_us = task_now_us - last_audio_task_us;
        if (gap_us > max_audio_task_gap_us) {
            max_audio_task_gap_us = gap_us;
        }
    }
    last_audio_task_us = task_now_us;

#if PICOARC_UAC_VERSION == 1
    update_feedback(false);
#endif

    const unsigned int bytes_per_sample = active_alt_uses_24_bit_subslot() ? 3u : 2u;
    const unsigned int bytes_per_frame = bytes_per_sample * CHANNELS;
    const bool gate_open = arc_audio_format_supported_quiet(active_alt, active_sample_rate);

    if (gate_open != audio_gate_open) {
        audio_gate_open = gate_open;
        output_enabled = false;
        refill_target_frames = START_BUFFER_FRAMES;
        spdif_clear_usb_buffer();
        spdif_set_mode(gate_open ? SPDIF_MODE_SILENCE : idle_spdif_mode());
#if PICOARC_UAC_VERSION == 2
        reset_adaptive_clock();
#endif
        printf("usb-audio: ARC gate %s (alt=%u, %lu Hz, %s)\n",
               gate_open ? "open" : "closed",
               active_alt,
               (unsigned long)active_sample_rate,
               active_alt_format_name());
    }

    while (tud_audio_available() >= bytes_per_frame) {
        uint16_t available = tud_audio_available();
        if (available > max_usb_available_bytes) {
            max_usb_available_bytes = available;
        }
        uint16_t bytes = available;
        if (bytes > sizeof(pcm_bytes)) {
            bytes = sizeof(pcm_bytes);
        }
        bytes = (uint16_t)(bytes - (bytes % bytes_per_frame));
        if (bytes == 0) {
            return;
        }

        uint16_t read = tud_audio_read(pcm_bytes, bytes);
#if PICOARC_USB_DIAGNOSTICS
        for (uint16_t i = 0; i < read; i++) {
            diagnostics_nonzero_bytes += pcm_bytes[i] != 0;
        }
#endif
        unsigned int frames = read / bytes_per_frame;
        const uint64_t read_now_us = time_us_64();
        if (last_audio_read_us != 0) {
            const uint64_t gap_us = read_now_us - last_audio_read_us;
            if (gap_us > max_audio_read_gap_us) {
                max_audio_read_gap_us = gap_us;
            }
        }
        last_audio_read_us = read_now_us;

        if (!gate_open) {
            gated_frames += frames;
#if PICOARC_USB_DIAGNOSTICS
            diagnostics_gated_frames += frames;
#endif
            continue;
        }

        // Decode complete little-endian USB subslots into the encoder's
        // 24-bit-left-aligned representation. 20-bit PCM is left-justified
        // within its 24-bit subslot, so its trailing padding is preserved.
        (void)audio_samples_decode_le(pcm_frames, frames * CHANNELS,
                                      pcm_bytes,
                                      frames * bytes_per_frame,
                                      bytes_per_sample);

        unsigned int written = spdif_write_pcm(pcm_frames, frames);
#if PICOARC_USB_DIAGNOSTICS
        diagnostics_written_frames += written;
#endif
        if (written < frames) {
            dropped_frames += frames - written;
        }
    }

    if (gate_open && !output_enabled && spdif_buffered_frames() >= refill_target_frames) {
        output_enabled = true;
        spdif_set_mode(SPDIF_MODE_USB_AUDIO);
#if PICOARC_UAC_VERSION == 2
        update_adaptive_clock(true);
#endif
        printf("usb-audio: output on buffered=%u\n", spdif_buffered_frames());
    }

    const uint64_t now_us = time_us_64();
    if (now_us >= next_diag_log_us) {
        spdif_usb_stats_t stats;
        spdif_take_usb_stats(&stats);
#if PICOARC_UAC_VERSION == 1
        printf("usb-audio: buf=%u lo=%u hi=%u under=%u dma-late=%u drop=%u gated=%u task-gap=%lluus read-gap=%lluus avail-hi=%u\n",
               stats.buffered_frames,
               stats.low_water_frames,
               stats.high_water_frames,
               stats.underrun_frames,
               stats.dma_late_blocks,
               dropped_frames,
               gated_frames,
               (unsigned long long)max_audio_task_gap_us,
               (unsigned long long)max_audio_read_gap_us,
               max_usb_available_bytes);
#else
        printf("usb-audio: buf=%u lo=%u hi=%u under=%u dma-late=%u drop=%u gated=%u task-gap=%lluus read-gap=%lluus avail-hi=%u clock=%ldppm\n",
               stats.buffered_frames,
               stats.low_water_frames,
               stats.high_water_frames,
               stats.underrun_frames,
               stats.dma_late_blocks,
               dropped_frames,
               gated_frames,
               (unsigned long long)max_audio_task_gap_us,
               (unsigned long long)max_audio_read_gap_us,
               max_usb_available_bytes,
               (long)adaptive_clock_adjust_ppm);
#endif
        if (output_enabled && stats.underrun_frames > 0) {
            output_enabled = false;
            refill_target_frames = RECOVER_BUFFER_FRAMES;
            spdif_set_mode(idle_spdif_mode());
#if PICOARC_UAC_VERSION == 2
            reset_adaptive_clock();
#endif
            printf("usb-audio: output paused for refill buffered=%u\n", spdif_buffered_frames());
        }
        dropped_frames = 0;
        gated_frames = 0;
        max_audio_task_gap_us = 0;
        max_audio_read_gap_us = 0;
        max_usb_available_bytes = 0;
        next_diag_log_us = now_us + 2000000;
    }

    if (!gate_open && now_us >= next_gate_log_us) {
        arc_audio_format_supported(active_alt, active_sample_rate);
        next_gate_log_us = now_us + 1000000;
    }

#if PICOARC_UAC_VERSION == 1
    update_feedback(false);
#else
    update_adaptive_clock(false);
#endif
}

#if PICOARC_USB_DIAGNOSTICS
bool usb_audio_take_hdmi_recovery_request(void) {
    const bool requested = diagnostics_hdmi_recovery_pending;
    diagnostics_hdmi_recovery_pending = false;
    return requested;
}

uint8_t usb_audio_take_isolated_recovery_request(void) {
    const uint8_t requested = diagnostics_isolated_pending;
    diagnostics_isolated_pending = 0;
    return requested;
}

static uint32_t audio_diagnostics_flags(void) {
    return (streaming ? 1u : 0u) |
           (output_enabled ? 2u : 0u) |
           (audio_gate_open ? 4u : 0u) |
           (tud_mounted() ? 8u : 0u) |
           (tud_suspended() ? 16u : 0u) |
           (arc_is_initiated() ? 32u : 0u) |
           (arc_system_audio_enabled() ? 64u : 0u) |
           (arc_audio_caps_ready() ? 128u : 0u) |
           (arc_hdmi_connected() ? 256u : 0u) |
           ((watchdog_hw->reason & WATCHDOG_REASON_TIMER_BITS) ? 512u : 0u) |
           ((watchdog_hw->reason & WATCHDOG_REASON_FORCE_BITS) ? 1024u : 0u) |
           2048u | 4096u;
}

void usb_audio_trace_state(void) {
    static bool initialized;
    static uint32_t previous_flags;
    static uint32_t previous_rate;
    static uint8_t previous_alt;
    static uint8_t previous_mode;
    const uint32_t flags = audio_diagnostics_flags();
    const uint8_t mode = spdif_get_mode();
    if (!initialized || flags != previous_flags || active_sample_rate != previous_rate ||
        active_alt != previous_alt || mode != previous_mode) {
        const uint8_t bytes[] = { active_alt, mode };
        diagnostics_trace_record((uint32_t)(time_us_64() / 1000u), DIAGNOSTICS_TRACE_STATE,
                                  flags, active_sample_rate, bytes, sizeof(bytes));
        initialized = true;
        previous_flags = flags;
        previous_rate = active_sample_rate;
        previous_alt = active_alt;
        previous_mode = mode;
    }
}

// Sixteen little-endian uint32_t values. Version 2 adds reset-cause flags;
// version 3 adds an output hardware query; version 4 adds an encoded-block
// snapshot; version 5 adds explicit targeted recovery and its status query.
// Version 6 adds HDMI recovery and read-only DDC/HPD/CEC status.
// Version 8 adds a retained event trace and isolated manual recovery commands.
// The base field layout is unchanged. Lifetime counters wrap naturally
// and are not cleared on a host query or streaming transition.
typedef struct {
    uint32_t version;
    uint32_t uptime_ms;
    uint32_t flags;
    uint32_t sample_rate;
    uint32_t active_alt;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t nonzero_bytes;
    uint32_t written_frames;
    uint32_t gated_frames;
    uint32_t buffered_frames;
    uint32_t spdif_mode;
    spdif_diagnostics_t spdif;
} audio_diagnostics_t;

_Static_assert(sizeof(audio_diagnostics_t) == 64, "Audio diagnostics wire size changed");
_Static_assert(sizeof(spdif_output_diagnostics_t) == 64, "Output diagnostics wire size changed");
_Static_assert(sizeof(spdif_block_diagnostics_t) == 3136, "Block diagnostics wire size changed");

typedef struct {
    uint32_t version;
    spdif_restart_diagnostics_t output;
    uint32_t usb_reconnect_requests;
    uint32_t last_usb_reconnect_ms;
    uint32_t hdmi_recovery_requests;
    uint32_t last_hdmi_recovery_ms;
    uint32_t isolated_requests;
    uint32_t last_isolated_ms;
    uint32_t last_isolated_action;
    uint32_t reserved[4];
} audio_recovery_diagnostics_t;
_Static_assert(sizeof(audio_recovery_diagnostics_t) == 64, "Recovery diagnostics wire size changed");

typedef struct {
    uint32_t version;
    uint32_t hpd_gpio_ctrl;
    uint32_t hpd_gpio_status;
    uint32_t gpio_levels;
    uint32_t cec_audio_status;
    ddc_edid_diagnostics_t ddc;
} hdmi_diagnostics_t;
_Static_assert(sizeof(hdmi_diagnostics_t) == 64, "HDMI diagnostics wire size changed");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    // The explicit BOOTSEL command has no payload or parameters. Complete
    // its status stage before leaving the running application.
    if (request->bmRequestType == 0x40 && request->bRequest == 0x5c &&
        request->wValue == 0 && request->wIndex == 0 && request->wLength == 0) {
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, request);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reset_usb_boot(0, 0);
        }
        return true;
    }

    // Explicit recovery: 1 restarts only the transmitter on core 1; 2 asks
    // the main adapter task to reconnect USB; 3 restarts HDMI negotiation and
    // the EDID responder; 4..7 isolate DDC, HPD, CEC messages and CEC RX.
    // Never mutate on a read/setup.
    if (request->bmRequestType == 0x40 && request->bRequest == 0x5e &&
        (request->wValue >= 1 && request->wValue <= 7) &&
        request->wIndex == 0 && request->wLength == 0) {
        if (stage == CONTROL_STAGE_SETUP) {
            if (request->wValue >= 4 && diagnostics_isolated_pending) {
                return false;
            }
            return tud_control_status(rhport, request);
        }
        if (stage == CONTROL_STAGE_ACK) {
            diagnostics_trace_record((uint32_t)(time_us_64() / 1000u),
                                      DIAGNOSTICS_TRACE_RECOVERY, 0, request->wValue, NULL, 0);
            if (request->wValue == 1) {
                spdif_request_output_restart();
            } else if (request->wValue == 2) {
                diagnostics_usb_reconnect_requests++;
                diagnostics_usb_reconnect_uptime_ms = (uint32_t)(time_us_64() / 1000u);
                diagnostics_usb_reconnect_pending = true;
            } else if (request->wValue == 3) {
                diagnostics_hdmi_recovery_requests++;
                diagnostics_hdmi_recovery_uptime_ms = (uint32_t)(time_us_64() / 1000u);
                diagnostics_hdmi_recovery_pending = true;
            } else {
                diagnostics_isolated_requests++;
                diagnostics_isolated_uptime_ms = (uint32_t)(time_us_64() / 1000u);
                diagnostics_last_isolated_action = request->wValue;
                diagnostics_isolated_pending = request->wValue;
            }
        }
        return true;
    }

    // Diagnostic reads are device-to-host, vendor, device recipient only.
    // Unsupported requests stall without changing state.
    if (request->bmRequestType != 0xc0 ||
        (request->bRequest != 0x5a && request->bRequest != 0x5b &&
         request->bRequest != 0x5d && request->bRequest != 0x5f &&
         request->bRequest != 0x60 && request->bRequest != 0x61) ||
        request->wValue != 0 || request->wIndex != 0 ||
        request->wLength == 0 ||
        request->wLength > (request->bRequest == 0x61 ? sizeof(diagnostics_trace_snapshot_t) :
            (request->bRequest == 0x5d ? sizeof(spdif_block_diagnostics_t) : sizeof(audio_diagnostics_t)))) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == 0x5b) {
        static spdif_output_diagnostics_t output;
        spdif_get_output_diagnostics(&output);
        return tud_control_xfer(rhport, request, &output, sizeof(output));
    }

    if (request->bRequest == 0x5d) {
        static spdif_block_diagnostics_t block;
        spdif_get_block_diagnostics(&block);
        return tud_control_xfer(rhport, request, &block, sizeof(block));
    }

    if (request->bRequest == 0x5f) {
        static audio_recovery_diagnostics_t recovery;
        recovery = (audio_recovery_diagnostics_t) {
            .version = 4,
            .usb_reconnect_requests = diagnostics_usb_reconnect_requests,
            .last_usb_reconnect_ms = diagnostics_usb_reconnect_uptime_ms,
            .hdmi_recovery_requests = diagnostics_hdmi_recovery_requests,
            .last_hdmi_recovery_ms = diagnostics_hdmi_recovery_uptime_ms,
            .isolated_requests = diagnostics_isolated_requests,
            .last_isolated_ms = diagnostics_isolated_uptime_ms,
            .last_isolated_action = diagnostics_last_isolated_action,
        };
        spdif_get_restart_diagnostics(&recovery.output);
        return tud_control_xfer(rhport, request, &recovery, sizeof(recovery));
    }

    if (request->bRequest == 0x60) {
        static hdmi_diagnostics_t hdmi;
        hdmi = (hdmi_diagnostics_t) {
            .version = 1,
            .hpd_gpio_ctrl = io_bank0_hw->io[PICOARC_HPD_GATE_PIN].ctrl,
            .hpd_gpio_status = io_bank0_hw->io[PICOARC_HPD_GATE_PIN].status,
            .gpio_levels = gpio_get_all(),
            .cec_audio_status = arc_get_audio_status_diagnostics(),
        };
        ddc_edid_get_diagnostics(&hdmi.ddc);
        return tud_control_xfer(rhport, request, &hdmi, sizeof(hdmi));
    }

    if (request->bRequest == 0x61) {
        static diagnostics_trace_snapshot_t trace;
        diagnostics_trace_snapshot((uint32_t)(time_us_64() / 1000u), &trace);
        return tud_control_xfer(rhport, request, &trace, sizeof(trace));
    }

    // Keep the response alive through the asynchronous EP0 transfer. Do not
    // take a core-1 lock: the query must work when the output core is wedged.
    static audio_diagnostics_t diagnostics;
    diagnostics = (audio_diagnostics_t) {
        .version = 8,
        .uptime_ms = (uint32_t)(time_us_64() / 1000u),
        .flags = audio_diagnostics_flags(),
        .sample_rate = active_sample_rate,
        .active_alt = active_alt,
        .rx_packets = diagnostics_rx_packets,
        .rx_bytes = diagnostics_rx_bytes,
        .nonzero_bytes = diagnostics_nonzero_bytes,
        .written_frames = diagnostics_written_frames,
        .gated_frames = diagnostics_gated_frames,
        .buffered_frames = spdif_buffered_frames(),
        .spdif_mode = spdif_get_mode(),
    };
    spdif_get_diagnostics(&diagnostics.spdif);
    return tud_control_xfer(rhport, request, &diagnostics, sizeof(diagnostics));
}
#endif
