#include "usb_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/structs/usb.h"
#include "pico/time.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_descriptors.h"

enum {
    SAMPLE_RATE = 48000,
    CHANNELS = 2,
    BYTES_PER_SAMPLE = 2,
    BYTES_PER_FRAME = CHANNELS * BYTES_PER_SAMPLE,
    FRAMES_PER_MS = SAMPLE_RATE / 1000,
    START_BUFFER_FRAMES = 4096,
    RECOVER_BUFFER_FRAMES = 4096,
    AUDIO_OUT_EP_NUM = 3,
    FEEDBACK_TARGET_FRAMES = 4096,
    FEEDBACK_UPDATE_US = 4000,
    FEEDBACK_P_GAIN_Q16_PER_FRAME = 16,
    FEEDBACK_MAX_ADJUST_Q16 = 1 << 15,
};

static int8_t mute[CHANNELS + 1];
static int16_t volume[CHANNELS + 1];
static int16_t pcm_buffer[FRAMES_PER_MS * CHANNELS * 2];
static bool streaming;
static bool output_enabled;
static unsigned int refill_target_frames;
static unsigned int dropped_frames;
static uint64_t next_diag_log_us;
static uint64_t next_feedback_us;

static int16_t cec_volume_to_usb_volume(uint8_t cec_volume) {
    if (cec_volume > 100) {
        cec_volume = 100;
    }

    const int32_t min_db_256 = -60 * 256;
    const int32_t span_db_256 = 60 * 256;
    return (int16_t)(min_db_256 + ((int32_t)cec_volume * span_db_256) / 100);
}

void usb_audio_set_cec_audio_status(uint8_t cec_volume, bool muted) {
    const int8_t usb_mute = muted ? 1 : 0;
    const int16_t usb_volume = cec_volume_to_usb_volume(cec_volume);

    for (unsigned int i = 0; i <= CHANNELS; i++) {
        mute[i] = usb_mute;
        volume[i] = usb_volume;
    }
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

    const uint32_t feedback_q16 = ((uint32_t)FRAMES_PER_MS << 16) + (uint32_t)adjust_q16;
    tud_audio_n_fb_set(0, feedback_q16);
    next_feedback_us = now_us + FEEDBACK_UPDATE_US;
}

static bool clock_get_request(uint8_t rhport, audio_control_request_t const *request) {
    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(SAMPLE_RATE)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur, sizeof(cur));
    }

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_RANGE) {
        audio_control_range_4_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {
                .bMin = (int32_t)tu_htole32(SAMPLE_RATE),
                .bMax = (int32_t)tu_htole32(SAMPLE_RATE),
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
                .bMin = tu_htole16(-60 * 256),
                .bMax = tu_htole16(0),
                .bRes = tu_htole16(256),
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
        return true;
    }

    if (request->bEntityID != UAC2_ENTITY_FEATURE_UNIT) {
        return false;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        return true;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
        return true;
    }

    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING) {
        streaming = alt != 0;
        output_enabled = false;
        refill_target_frames = START_BUFFER_FRAMES;
        spdif_clear_usb_buffer();
        spdif_set_mode(SPDIF_MODE_SILENCE);
        dropped_frames = 0;
        next_diag_log_us = time_us_64() + 2000000;
        next_feedback_us = 0;
        update_feedback(true);
        printf("usb-audio: streaming %s, spdif=%s\n", streaming ? "on" : "off", spdif_mode_name(spdif_get_mode()));
    }

    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *request) {
    (void)rhport;
    (void)request;
    streaming = false;
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
    feedback_param->sample_freq = SAMPLE_RATE;
}

void usb_audio_task(void) {
    if (!streaming) {
        return;
    }

    update_feedback(false);

    while (tud_audio_available() >= BYTES_PER_FRAME) {
        uint16_t available = tud_audio_available();
        uint16_t bytes = available;
        if (bytes > sizeof(pcm_buffer)) {
            bytes = sizeof(pcm_buffer);
        }
        bytes = (uint16_t)(bytes - (bytes % BYTES_PER_FRAME));
        if (bytes == 0) {
            return;
        }

        uint16_t read = tud_audio_read(pcm_buffer, bytes);
        unsigned int frames = read / BYTES_PER_FRAME;
        unsigned int written = spdif_write_pcm(pcm_buffer, frames);
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
