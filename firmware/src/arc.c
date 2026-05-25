#include "arc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cec.h"
#include "ddc_edid.h"
#include "hardware/gpio.h"
#include "picoarc_config.h"
#include "picoarc_log.h"
#include "pico/stdlib.h"
#include "spdif.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

static unsigned int hdmi_5v_pin;
static bool arc_initiated;
static bool system_audio_mode;
static bool last_streaming;
static bool last_source_5v;
static unsigned int probe_step;
static unsigned int streaming_probe_step;
static unsigned int logical_scan_address;
static uint16_t logical_scan_seen_mask;
static bool logical_scan_started;
static bool logical_scan_done;
static bool logical_scan_diagnostic;
static bool ddc_cec_wait_logged;
static absolute_time_t next_logical_scan;
static unsigned int device_info_probe_step;
static bool device_info_probe_done;
static absolute_time_t next_probe;
static absolute_time_t next_volume_sync;
static uint64_t next_stream_audio_log_us;
static unsigned int suppressed_stream_audio_logs;
static bool cec_audio_volume_known;
static bool cec_audio_mute_known;
static uint8_t cec_audio_volume;
static bool cec_audio_muted;
static bool absolute_volume_supported;
static bool absolute_volume_verify_pending;
static uint8_t absolute_volume_verify_target;
static bool pending_volume_sync;
static uint8_t pending_volume;
static bool pending_mute_sync;
static bool pending_mute;
static bool relative_volume_sync_active;
static uint8_t relative_volume_target;
static bool relative_volume_key_held;
static uint8_t relative_volume_held_key;
static char soundbar_osd_name[15];
static bool soundbar_name_probe_started;
static bool soundbar_name_ready;
static absolute_time_t soundbar_name_deadline;

typedef enum {
    SAD_QUERY_NOT_STARTED,
    SAD_QUERY_REQUESTED,
    SAD_QUERY_REPORTED,
    SAD_QUERY_UNSUPPORTED,
    SAD_QUERY_TIMEOUT,
} sad_query_state_t;

static sad_query_state_t sad_query_state;
static absolute_time_t sad_query_deadline;
static uint8_t sad_pcm_rates_16;
static uint8_t sad_pcm_rates_20;
static uint8_t sad_pcm_rates_24;
static uint8_t sad_ac3_rates;
static uint8_t sad_dts_rates;
static unsigned int sad_extra_query_index;
static bool sad_extra_query_requested;
static absolute_time_t sad_extra_query_deadline;

static const unsigned int max_cec_frames_per_task = 8;
static const unsigned int streaming_volume_sync_debounce_ms = 250;
static const unsigned int soundbar_name_response_timeout_ms = 2000;
static const unsigned int logical_scan_interval_ms = 150;
static const unsigned int logical_scan_retry_ms = 5000;
static const uint16_t tv_physical_address = 0x0000;

typedef struct {
    const char *label;
    uint8_t count;
    uint8_t operands[4];
} sad_query_batch_t;

static const sad_query_batch_t sad_extra_queries[] = {
    {"MPEG/AAC", 4, {0x03, 0x04, 0x05, 0x06}},
    {"ATRAC/OneBit/EAC3/DTS-HD", 4, {0x08, 0x09, 0x0a, 0x0b}},
    {"MLP/DST/WMAPro", 3, {0x0c, 0x0d, 0x0e}},
    {"CEA extensions", 3, {0x41, 0x42, 0x43}},
};

static uint8_t sample_rate_to_sad_bit(uint32_t sample_rate) {
    switch (sample_rate) {
    case PICOARC_AUDIO_SAMPLE_RATE_32K:
        return PICOARC_AUDIO_RATE_BIT_32K;
    case PICOARC_AUDIO_SAMPLE_RATE_44K1:
        return PICOARC_AUDIO_RATE_BIT_44K1;
    case PICOARC_AUDIO_SAMPLE_RATE_48K:
        return PICOARC_AUDIO_RATE_BIT_48K;
    case PICOARC_AUDIO_SAMPLE_RATE_88K2:
        return PICOARC_AUDIO_RATE_BIT_88K2;
    case PICOARC_AUDIO_SAMPLE_RATE_96K:
        return PICOARC_AUDIO_RATE_BIT_96K;
    default:
        return 0;
    }
}

static const char *opcode_name(uint8_t opcode) {
    switch (opcode) {
    case 0x00:
        return "feature-abort";
    case 0x04:
        return "image-view-on";
    case 0x0d:
        return "text-view-on";
    case 0x1a:
        return "give-deck-status";
    case 0x1b:
        return "deck-status";
    case 0x36:
        return "standby";
    case 0x41:
        return "play";
    case 0x42:
        return "deck-control";
    case 0x44:
        return "user-control-pressed";
    case 0x45:
        return "user-control-released";
    case 0x46:
        return "give-osd-name";
    case 0x47:
        return "set-osd-name";
    case 0x70:
        return "system-audio-mode-request";
    case 0x71:
        return "give-audio-status";
    case 0x72:
        return "set-system-audio-mode";
    case 0x73:
        return "set-audio-volume-level";
    case 0x7a:
        return "report-audio-status";
    case 0x7d:
        return "give-system-audio-mode-status";
    case 0x7e:
        return "system-audio-mode-status";
    case 0x82:
        return "active-source";
    case 0x83:
        return "give-physical-address";
    case 0x84:
        return "report-physical-address";
    case 0x85:
        return "request-active-source";
    case 0x86:
        return "set-stream-path";
    case 0x87:
        return "device-vendor-id";
    case 0x89:
        return "vendor-command";
    case 0x8c:
        return "give-device-vendor-id";
    case 0x8d:
        return "menu-request";
    case 0x8e:
        return "menu-status";
    case 0x8f:
        return "give-device-power-status";
    case 0x90:
        return "report-power-status";
    case 0x9e:
        return "cec-version";
    case 0x9f:
        return "get-cec-version";
    case 0xa0:
        return "vendor-command-with-id";
    case 0xa3:
        return "report-short-audio-descriptor";
    case 0xa4:
        return "request-short-audio-descriptor";
    case 0xa7:
        return "request-current-latency";
    case 0xa8:
        return "report-current-latency";
    case 0xc0:
        return "initiate-arc";
    case 0xc1:
        return "report-arc-initiated";
    case 0xc2:
        return "report-arc-terminated";
    case 0xc3:
        return "request-arc-initiation";
    case 0xc4:
        return "request-arc-termination";
    case 0xc5:
        return "terminate-arc";
    default:
        return "unknown";
    }
}

static const char *feature_abort_reason_name(uint8_t reason) {
    switch (reason) {
    case 0x00:
        return "unrecognized-opcode";
    case 0x01:
        return "not-in-correct-mode";
    case 0x02:
        return "cannot-provide-source";
    case 0x03:
        return "invalid-operand";
    case 0x04:
        return "refused";
    default:
        return "unknown";
    }
}

static const char *device_type_name(uint8_t type) {
    switch (type) {
    case 0x00:
        return "tv";
    case 0x01:
        return "recording";
    case 0x03:
        return "tuner";
    case 0x04:
        return "playback";
    case 0x05:
        return "audio-system";
    case 0x06:
        return "switch";
    case 0x07:
        return "video-processor";
    default:
        return "unknown";
    }
}

static const char *cec_version_name(uint8_t version) {
    switch (version) {
    case 0x04:
        return "1.3a";
    case 0x05:
        return "1.4";
    case 0x06:
        return "2.0";
    default:
        return "unknown";
    }
}

static const char *logical_address_name(uint8_t address) {
    switch (address) {
    case 0x0:
        return "tv";
    case 0x1:
        return "recording-1";
    case 0x2:
        return "recording-2";
    case 0x3:
        return "tuner-1";
    case 0x4:
        return "playback-1";
    case 0x5:
        return "audio-system";
    case 0x6:
        return "tuner-2";
    case 0x7:
        return "tuner-3";
    case 0x8:
        return "playback-2";
    case 0x9:
        return "recording-3";
    case 0xa:
        return "tuner-4";
    case 0xb:
        return "playback-3";
    case 0xc:
        return "reserved-1";
    case 0xd:
        return "reserved-2";
    case 0xe:
        return "free-use";
    case 0xf:
        return "broadcast";
    default:
        return "unknown";
    }
}

static const char *user_control_name(uint8_t code) {
    switch (code) {
    case 0x00:
        return "select";
    case 0x01:
        return "up";
    case 0x02:
        return "down";
    case 0x03:
        return "left";
    case 0x04:
        return "right";
    case 0x0d:
        return "exit";
    case 0x30:
        return "channel-up";
    case 0x31:
        return "channel-down";
    case 0x35:
        return "input-select";
    case 0x41:
        return "volume-up";
    case 0x42:
        return "volume-down";
    case 0x43:
        return "mute";
    case 0x44:
        return "play";
    case 0x45:
        return "stop";
    case 0x46:
        return "pause";
    case 0x48:
        return "rewind";
    case 0x49:
        return "fast-forward";
    case 0x4b:
        return "forward";
    case 0x4c:
        return "backward";
    case 0x60:
        return "play-function";
    case 0x61:
        return "pause-play-function";
    case 0x64:
        return "stop-function";
    case 0x65:
        return "mute-function";
    default:
        return "unknown";
    }
}

static void print_physical_address(uint16_t physical_address) {
    printf("%x.%x.%x.%x(0x%04x)",
           (physical_address >> 12) & 0x0f,
           (physical_address >> 8) & 0x0f,
           (physical_address >> 4) & 0x0f,
           physical_address & 0x0f,
           physical_address);
}

static const char *latency_compensation_name(uint8_t audio_out_compensated) {
    switch (audio_out_compensated) {
    case 0:
        return "n/a";
    case 1:
        return "delay";
    case 2:
        return "no-delay";
    case 3:
        return "partial-delay";
    default:
        return "invalid";
    }
}

static void print_frame(const cec_frame_t *frame) {
    if (frame->len == 0) {
        return;
    }

    const uint8_t header = frame->bytes[0];
    printf("cec: rx +%llums %x->%x",
           (unsigned long long)(time_us_64() / 1000),
           header >> 4,
           header & 0x0f);
    for (size_t i = 1; i < frame->len; i++) {
        printf(" %02x", frame->bytes[i]);
    }

    if (frame->len >= 2) {
        printf(" <%s>", opcode_name(frame->bytes[1]));
        if (frame->bytes[1] == 0x7a && frame->len >= 3) {
            const bool muted = (frame->bytes[2] & 0x80) != 0;
            const uint8_t volume = frame->bytes[2] & 0x7f;
            printf(" volume=%u muted=%s", volume, muted ? "yes" : "no");
        } else if (frame->bytes[1] == 0x00 && frame->len >= 4) {
            printf(" failed=%s(0x%02x) reason=%s(0x%02x)",
                   opcode_name(frame->bytes[2]),
                   frame->bytes[2],
                   feature_abort_reason_name(frame->bytes[3]),
                   frame->bytes[3]);
        } else if (frame->bytes[1] == 0x84 && frame->len >= 5) {
            const uint16_t physical_address = ((uint16_t)frame->bytes[2] << 8) | frame->bytes[3];
            const uint8_t device_type = frame->bytes[4];
            printf(" pa=");
            print_physical_address(physical_address);
            printf(" type=%s(0x%02x)",
                   device_type_name(device_type),
                   device_type);
        } else if (frame->bytes[1] == 0x87 && frame->len >= 5) {
            const uint32_t vendor_id = ((uint32_t)frame->bytes[2] << 16) |
                                       ((uint32_t)frame->bytes[3] << 8) |
                                       frame->bytes[4];
            printf(" vendor-id=0x%06lx", (unsigned long)vendor_id);
        } else if (frame->bytes[1] == 0x9e && frame->len >= 3) {
            printf(" version=%s(0x%02x)", cec_version_name(frame->bytes[2]), frame->bytes[2]);
        } else if (frame->bytes[1] == 0x47 && frame->len >= 3) {
            printf(" name=\"");
            for (size_t i = 2; i < frame->len; i++) {
                const char c = (char)frame->bytes[i];
                putchar(c >= 0x20 && c <= 0x7e ? c : '.');
            }
            printf("\"");
        } else if (frame->bytes[1] == 0xa7 && frame->len >= 4) {
            const uint16_t physical_address = ((uint16_t)frame->bytes[2] << 8) | frame->bytes[3];
            printf(" pa=");
            print_physical_address(physical_address);
        } else if (frame->bytes[1] == 0xa8 && frame->len >= 6) {
            const uint16_t physical_address = ((uint16_t)frame->bytes[2] << 8) | frame->bytes[3];
            const uint8_t video_latency = frame->bytes[4];
            const bool low_latency_mode = ((frame->bytes[5] >> 2) & 1u) != 0;
            const uint8_t audio_out_compensated = frame->bytes[5] & 0x03;
            printf(" pa=");
            print_physical_address(physical_address);
            printf(" video-latency=%ums low-latency-mode=%s audio-out=%s(0x%02x)",
                   video_latency,
                   low_latency_mode ? "yes" : "no",
                   latency_compensation_name(audio_out_compensated),
                   audio_out_compensated);
            if (audio_out_compensated == 3 && frame->len >= 7) {
                printf(" audio-delay=%ums", frame->bytes[6]);
            }
        } else if (frame->bytes[1] == 0x44 && frame->len >= 3) {
            printf(" key=%s(0x%02x)", user_control_name(frame->bytes[2]), frame->bytes[2]);
        } else if ((frame->bytes[1] == 0x72 || frame->bytes[1] == 0x7e) && frame->len >= 3) {
            printf(" enabled=%s", frame->bytes[2] ? "yes" : "no");
        } else if (frame->bytes[1] == 0x1a && frame->len >= 3) {
            printf(" status-request=0x%02x", frame->bytes[2]);
        } else if (frame->bytes[1] == 0x1b && frame->len >= 3) {
            printf(" deck-info=0x%02x", frame->bytes[2]);
        }
    }
    printf("\n");
}

static bool should_print_frame(const cec_frame_t *frame, bool streaming) {
    if (!streaming || frame->len < 2 || frame->bytes[1] != 0x7a) {
        return true;
    }

    const uint64_t now_us = time_us_64();
    if (now_us < next_stream_audio_log_us) {
        suppressed_stream_audio_logs++;
        return false;
    }

    if (suppressed_stream_audio_logs > 0) {
        printf("cec: suppressed %u audio-status logs while streaming\n", suppressed_stream_audio_logs);
        suppressed_stream_audio_logs = 0;
    }
    next_stream_audio_log_us = now_us + 500000;
    return true;
}

static bool send_report_physical_address(uint8_t source, uint16_t physical_address, uint8_t device_type) {
    const uint8_t msg[] = {
        (source << 4) | CEC_LOGICAL_BROADCAST,
        0x84,
        physical_address >> 8,
        physical_address & 0xff,
        device_type,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_cec_version(uint8_t source, uint8_t destination) {
    const uint8_t msg[] = {
        (source << 4) | destination,
        0x9e,
        0x06,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_power_status_on(uint8_t source, uint8_t destination) {
    const uint8_t msg[] = {
        (source << 4) | destination,
        0x90,
        0x00,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_device_vendor_id(uint8_t source) {
    const uint8_t msg[] = {
        (source << 4) | CEC_LOGICAL_BROADCAST,
        0x87,
        0x00,
        0x00,
        0x00,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_set_osd_name(uint8_t source, uint8_t destination, const char *name) {
    uint8_t msg[16] = {
        (source << 4) | destination,
        0x47,
    };
    size_t name_len = strlen(name);
    if (name_len > sizeof(msg) - 2) {
        name_len = sizeof(msg) - 2;
    }
    memcpy(&msg[2], name, name_len);
    return cec_send_with_retry(msg, name_len + 2, 2);
}

static bool send_feature_abort(uint8_t source, uint8_t destination, uint8_t failed_opcode, uint8_t reason) {
    const uint8_t msg[] = {
        (source << 4) | destination,
        0x00,
        failed_opcode,
        reason,
    };
    return cec_send_with_retry(msg, sizeof(msg), 1);
}

static bool send_system_audio_mode_request(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x70,
        tv_physical_address >> 8,
        tv_physical_address & 0xff,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_give_audio_status(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x71,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_give_osd_name(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x46,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_give_device_vendor_id(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x8c,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_get_cec_version(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x9f,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_request_current_latency(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_BROADCAST,
        0xa7,
        tv_physical_address >> 8,
        tv_physical_address & 0xff,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_request_short_audio_descriptor_operands(const uint8_t *operands, size_t count) {
    if (count > 4) {
        count = 4;
    }

    uint8_t msg[6] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xa4,
    };
    memcpy(&msg[2], operands, count);
    return cec_send_with_retry(msg, count + 2, 2);
}

static bool send_request_short_audio_descriptor(void) {
    static const uint8_t primary_operands[] = {
        0x01, // LPCM
        0x02, // AC-3
        0x07, // DTS
    };
    return send_request_short_audio_descriptor_operands(primary_operands, sizeof(primary_operands));
}

static bool send_set_audio_volume_level(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }

    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x73,
        volume,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_user_control_pressed(uint8_t key) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x44,
        key,
    };
    return cec_send_with_retry(msg, sizeof(msg), 1);
}

static bool send_user_control_released(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x45,
    };
    return cec_send_with_retry(msg, sizeof(msg), 1);
}

static bool send_user_control_tap(uint8_t key) {
    const bool press_ack = send_user_control_pressed(key);
    cec_delay_ms(40);
    const bool release_ack = send_user_control_released();
    return press_ack && release_ack;
}

static bool release_relative_volume_key(void) {
    if (!relative_volume_key_held) {
        return true;
    }

    const bool ack = send_user_control_released();
    relative_volume_key_held = false;
    return ack;
}

static bool send_relative_volume_press(uint8_t key) {
    if (relative_volume_key_held && relative_volume_held_key != key) {
        release_relative_volume_key();
    }

    const bool ack = send_user_control_pressed(key);
    if (ack) {
        relative_volume_key_held = true;
        relative_volume_held_key = key;
    }
    return ack;
}

static bool send_give_system_audio_mode_status(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x7d,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_request_arc_initiation(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xc3,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_initiate_arc(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xc0,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_report_arc_initiated(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xc1,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool audio_is_streaming(void) {
    return usb_audio_is_streaming();
}

static const char *sad_format_name(uint8_t format_code) {
    switch (format_code) {
    case 1:
        return "LPCM";
    case 2:
        return "AC-3";
    case 3:
        return "MPEG-1";
    case 4:
        return "MP3";
    case 5:
        return "MPEG-2";
    case 6:
        return "AAC LC";
    case 7:
        return "DTS";
    case 8:
        return "ATRAC";
    case 9:
        return "One Bit Audio";
    case 10:
        return "E-AC-3";
    case 11:
        return "DTS-HD";
    case 12:
        return "MLP";
    case 13:
        return "DST";
    case 14:
        return "WMA Pro";
    case 15:
        return "Extension";
    default:
        return "unknown";
    }
}

static const char *sad_request_operand_name(uint8_t operand) {
    const uint8_t format_id = operand >> 6;
    const uint8_t format_code = operand & 0x3f;

    if (format_id == 0) {
        return sad_format_name(format_code);
    }

    if (format_id == 1) {
        switch (format_code) {
        case 1:
            return "HE-AAC";
        case 2:
            return "HE-AAC v2";
        case 3:
            return "MPEG Surround";
        default:
            return "CEA extension";
        }
    }

    return "unknown";
}

static void reset_sad_caps(void) {
    sad_pcm_rates_16 = 0;
    sad_pcm_rates_20 = 0;
    sad_pcm_rates_24 = 0;
    sad_ac3_rates = 0;
    sad_dts_rates = 0;
}

static bool sad_has_usb_usable_format(void) {
    return ((sad_pcm_rates_16 | sad_pcm_rates_20 | sad_pcm_rates_24 |
             sad_ac3_rates | sad_dts_rates) & PICOARC_AUDIO_USB_RATE_MASK) != 0;
}

static void parse_short_audio_descriptors(const cec_frame_t *frame, bool update_core_caps) {
    if (update_core_caps) {
        reset_sad_caps();
    }

    const size_t payload_len = frame->len > 2 ? frame->len - 2 : 0;
    const size_t descriptor_count = payload_len / 3;
    if (descriptor_count == 0) {
        printf("arc: SAD report contained no descriptors\n");
    }
    for (size_t i = 0; i < descriptor_count; i++) {
        const uint8_t byte0 = frame->bytes[2 + i * 3];
        const uint8_t byte1 = frame->bytes[3 + i * 3];
        const uint8_t byte2 = frame->bytes[4 + i * 3];
        const uint8_t format_code = (byte0 >> 3) & 0x0f;
        const uint8_t channels = (byte0 & 0x07) + 1;
        const uint8_t rates = byte1 & 0x7f;

        if (format_code == 1) {
            const bool bits_16 = (byte2 & (1u << 0)) != 0;
            const bool bits_20 = (byte2 & (1u << 1)) != 0;
            const bool bits_24 = (byte2 & (1u << 2)) != 0;
            if (update_core_caps && channels >= 2 && bits_16) {
                sad_pcm_rates_16 |= rates;
            }
            if (update_core_caps && channels >= 2 && bits_20) {
                sad_pcm_rates_20 |= rates;
            }
            if (update_core_caps && channels >= 2 && bits_24) {
                sad_pcm_rates_24 |= rates;
            }
            printf("arc: SAD %s ch=%u rates=0x%02x bits=0x%02x\n",
                   sad_format_name(format_code),
                   channels,
                   byte1,
                   byte2);
        } else {
            if (update_core_caps && format_code == 2 && channels >= 2) {
                sad_ac3_rates |= rates;
            } else if (update_core_caps && format_code == 7 && channels >= 2) {
                sad_dts_rates |= rates;
            }
            if (format_code == 2 || format_code == 7) {
                printf("arc: SAD %s ch=%u rates=0x%02x max=%ukbps\n",
                       sad_format_name(format_code),
                       channels,
                       byte1,
                       (unsigned int)byte2 * 8u);
            } else {
                printf("arc: SAD %s ch=%u rates=0x%02x data=0x%02x\n",
                       sad_format_name(format_code),
                       channels,
                       byte1,
                       byte2);
            }
        }
    }

    if (update_core_caps) {
        sad_query_state = SAD_QUERY_REPORTED;
        printf("arc: audio caps pcm16=0x%02x pcm20=0x%02x pcm24=0x%02x ac3=0x%02x dts=0x%02x usb-mask=0x%02x\n",
               sad_pcm_rates_16,
               sad_pcm_rates_20,
               sad_pcm_rates_24,
               sad_ac3_rates,
               sad_dts_rates,
               PICOARC_AUDIO_USB_RATE_MASK);
        if (!sad_has_usb_usable_format()) {
            printf("arc: no USB-advertised ARC audio formats reported; streaming will be refused\n");
        }
    }
}

static void reset_sad_extra_queries(void) {
    sad_extra_query_index = 0;
    sad_extra_query_requested = false;
    sad_extra_query_deadline = make_timeout_time_ms(0);
}

static void reset_soundbar_osd_name(void) {
    soundbar_osd_name[0] = '\0';
    soundbar_name_probe_started = false;
    soundbar_name_ready = false;
    soundbar_name_deadline = make_timeout_time_ms(0);
    usb_descriptors_reset_audio_name();
}

static bool soundbar_name_ready_for_usb(void) {
#if PICOARC_DEBUG_USB
    return true;
#else
    return soundbar_name_ready;
#endif
}

static bool request_soundbar_osd_name(bool source_5v, bool bus_high, const char *prefix) {
    if (soundbar_name_ready || soundbar_name_probe_started) {
        return false;
    }

    soundbar_name_probe_started = true;
    soundbar_name_deadline = make_timeout_time_ms(soundbar_name_response_timeout_ms);

    const bool ack = send_give_osd_name();
    printf("arc: %sgive-osd-name ack=%s src5v=%d idle=%d\n",
           prefix,
           ack ? "yes" : "no",
           source_5v,
           bus_high);
    return ack;
}

static void update_soundbar_name_timeout(void) {
    if (!soundbar_name_probe_started || soundbar_name_ready ||
        absolute_time_diff_us(get_absolute_time(), soundbar_name_deadline) > 0) {
        return;
    }

    soundbar_name_ready = true;
    printf("arc: soundbar OSD name unavailable; USB audio descriptor stays default\n");
}

static void capture_soundbar_osd_name(const cec_frame_t *frame) {
    char name[sizeof(soundbar_osd_name)];
    size_t out = 0;

    for (size_t i = 2; i < frame->len && out < sizeof(name) - 1; i++) {
        const uint8_t raw = frame->bytes[i];
        const char c = (raw >= 0x20 && raw <= 0x7e) ? (char)raw : ' ';
        if (c == ' ' && out == 0) {
            continue;
        }
        name[out++] = c;
    }

    while (out > 0 && name[out - 1] == ' ') {
        out--;
    }
    name[out] = '\0';

    soundbar_name_ready = true;
    if (out == 0) {
        printf("arc: soundbar OSD name was empty; USB audio descriptor stays default\n");
        return;
    }

    memcpy(soundbar_osd_name, name, out + 1);
    usb_descriptors_set_audio_name(soundbar_osd_name);
    printf("arc: soundbar OSD name=\"%s\"; USB audio descriptor will use it\n", soundbar_osd_name);
}

static void reset_device_info_probe(void) {
    device_info_probe_step = 0;
    device_info_probe_done = false;
    reset_soundbar_osd_name();
}

static void reset_logical_address_scan(void) {
    logical_scan_address = 1;
    logical_scan_seen_mask = 0;
    logical_scan_started = false;
    logical_scan_done = false;
    logical_scan_diagnostic = false;
    next_logical_scan = make_timeout_time_ms(0);
}

static bool logical_scan_any_device_seen(void) {
    return (logical_scan_seen_mask & ~(1u << CEC_LOGICAL_TV)) != 0;
}

static bool logical_scan_audio_system_seen(void) {
    return (logical_scan_seen_mask & (1u << CEC_LOGICAL_AUDIO_SYSTEM)) != 0;
}

static void note_logical_address_seen(uint8_t address) {
    if (address < CEC_LOGICAL_BROADCAST) {
        logical_scan_seen_mask |= (uint16_t)(1u << address);
    }
}

static bool service_logical_address_scan(bool source_5v, bool bus_high) {
    if (!logical_scan_done && logical_scan_audio_system_seen()) {
        logical_scan_done = true;
        next_probe = make_timeout_time_ms(100);
        printf("arc: logical-address acquisition complete mask=0x%04x audio-system=yes src5v=%d idle=%d\n",
               logical_scan_seen_mask,
               source_5v,
               bus_high);
        return false;
    }

    if (logical_scan_done) {
        if (logical_scan_audio_system_seen()) {
            return false;
        }

        if (absolute_time_diff_us(get_absolute_time(), next_logical_scan) > 0) {
            return true;
        }

        printf("arc: retrying logical-address scan src5v=%d idle=%d\n",
               source_5v,
               bus_high);
        reset_logical_address_scan();
    }

    if (absolute_time_diff_us(get_absolute_time(), next_logical_scan) > 0) {
        return true;
    }

    if (!logical_scan_started) {
        logical_scan_started = true;
        printf("arc: audio-system logical-address probe start src5v=%d idle=%d\n",
               source_5v,
               bus_high);
    }

    if (!logical_scan_diagnostic) {
        const bool ack = cec_poll(CEC_LOGICAL_TV, CEC_LOGICAL_AUDIO_SYSTEM);
        if (ack) {
            note_logical_address_seen(CEC_LOGICAL_AUDIO_SYSTEM);
            logical_scan_done = true;
            next_probe = make_timeout_time_ms(100);
        } else {
            logical_scan_diagnostic = true;
            logical_scan_address = 1;
            next_logical_scan = make_timeout_time_ms(logical_scan_interval_ms);
        }

        printf("arc: audio-system probe ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        if (!ack) {
            printf("arc: audio-system silent; starting diagnostic logical-address scan src5v=%d idle=%d\n",
                   source_5v,
                   bus_high);
        }
        return true;
    }

    while (logical_scan_address == CEC_LOGICAL_TV ||
           logical_scan_address == CEC_LOGICAL_BROADCAST) {
        logical_scan_address++;
    }

    if (logical_scan_address >= CEC_LOGICAL_BROADCAST) {
        logical_scan_done = true;
        if (logical_scan_audio_system_seen()) {
            next_probe = make_timeout_time_ms(250);
            printf("arc: logical-address scan complete mask=0x%04x audio-system=yes src5v=%d idle=%d\n",
                   logical_scan_seen_mask,
                   source_5v,
                   bus_high);
        } else {
            next_logical_scan = make_timeout_time_ms(logical_scan_retry_ms);
            printf("arc: logical-address scan complete mask=0x%04x audio-system=no; holding ARC probes and retrying in %ums src5v=%d idle=%d\n",
                   logical_scan_seen_mask,
                   logical_scan_retry_ms,
                   source_5v,
                   bus_high);
            if (!logical_scan_any_device_seen()) {
                printf("arc: no CEC devices answered; check CEC continuity, HPD, and whether the soundbar requires DDC/EDID\n");
            }
        }
        return true;
    }

    const uint8_t address = (uint8_t)logical_scan_address++;
    const bool ack = cec_poll(CEC_LOGICAL_TV, address);
    if (ack) {
        note_logical_address_seen(address);
    }
    printf("arc: diagnostic logical-address scan addr=%x(%s) ack=%s src5v=%d idle=%d\n",
           address,
           logical_address_name(address),
           ack ? "yes" : "no",
           source_5v,
           bus_high);
    next_logical_scan = make_timeout_time_ms(logical_scan_interval_ms);
    return true;
}

static bool sad_query_finished(void) {
    return sad_query_state == SAD_QUERY_REPORTED ||
           sad_query_state == SAD_QUERY_UNSUPPORTED ||
           sad_query_state == SAD_QUERY_TIMEOUT;
}

static bool idle_probe_complete(void) {
    return arc_initiated &&
           system_audio_mode &&
           sad_query_finished() &&
           cec_audio_mute_known;
}

static const char *sad_query_state_name(void) {
    switch (sad_query_state) {
    case SAD_QUERY_NOT_STARTED:
        return "not-started";
    case SAD_QUERY_REQUESTED:
        return "requested";
    case SAD_QUERY_REPORTED:
        return "reported";
    case SAD_QUERY_UNSUPPORTED:
        return "unsupported";
    case SAD_QUERY_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

static void update_cec_audio_status(uint8_t status) {
    const bool muted = (status & 0x80) != 0;
    const uint8_t volume = status & 0x7f;
    const bool host_sync_active = pending_volume_sync ||
                                  pending_mute_sync ||
                                  relative_volume_sync_active ||
                                  absolute_volume_verify_pending;
    const bool notify_host = !host_sync_active;

    cec_audio_muted = muted;
    cec_audio_mute_known = true;
    if (volume <= 100) {
        cec_audio_volume = volume;
        cec_audio_volume_known = true;
        usb_audio_set_cec_audio_status(volume, muted, notify_host);
        if (absolute_volume_verify_pending) {
            absolute_volume_verify_pending = false;
            if (volume != absolute_volume_verify_target) {
                absolute_volume_supported = false;
                printf("arc: absolute volume ignored wanted=%u reported=%u; switching to relative control\n",
                       absolute_volume_verify_target,
                       volume);
                relative_volume_target = absolute_volume_verify_target;
                relative_volume_sync_active = true;
                next_volume_sync = make_timeout_time_ms(150);
            }
        }
    } else {
        usb_audio_set_cec_mute_status(muted, notify_host);
        printf("arc: audio-status volume unknown muted=%s\n", muted ? "yes" : "no");
    }
}

static bool start_relative_volume_sync(uint8_t desired_volume) {
    if (!cec_audio_volume_known) {
        return false;
    }

    relative_volume_target = desired_volume;
    relative_volume_sync_active = cec_audio_volume != desired_volume;
    next_volume_sync = make_timeout_time_ms(0);
    return true;
}

static bool continue_relative_volume_sync(bool source_5v, bool bus_high) {
    if (!relative_volume_sync_active || !cec_audio_volume_known) {
        return false;
    }

    if (cec_audio_volume == relative_volume_target) {
        relative_volume_sync_active = false;
        release_relative_volume_key();
        return false;
    }

    const uint8_t key = relative_volume_target > cec_audio_volume ? 0x41 : 0x42;
    const bool streaming = audio_is_streaming();
    const uint64_t tx_start_us = time_us_64();
    const bool ack = send_relative_volume_press(key);
    const uint64_t tx_end_us = time_us_64();
    if (ack) {
        cec_audio_volume += key == 0x41 ? 1 : -1;
    }

    if (streaming) {
        printf("arc: volume-press +%llums key=%s ack=%s dur=%lluus now=%u target=%u buf=%u\n",
               (unsigned long long)(tx_start_us / 1000),
               user_control_name(key),
               ack ? "yes" : "no",
               (unsigned long long)(tx_end_us - tx_start_us),
               cec_audio_volume,
               relative_volume_target,
               spdif_buffered_frames());
    } else {
        printf("arc: relative volume step key=%s now=%u target=%u ack=%s src5v=%d idle=%d\n",
               user_control_name(key),
               cec_audio_volume,
               relative_volume_target,
               ack ? "yes" : "no",
               source_5v,
               bus_high);
    }

    if (cec_audio_volume == relative_volume_target) {
        const uint64_t release_start_us = time_us_64();
        const bool release_ack = release_relative_volume_key();
        const uint64_t release_end_us = time_us_64();
        if (streaming) {
            printf("arc: volume-release +%llums ack=%s dur=%lluus buf=%u\n",
                   (unsigned long long)(release_start_us / 1000),
                   release_ack ? "yes" : "no",
                   (unsigned long long)(release_end_us - release_start_us),
                   spdif_buffered_frames());
        }
    }

    next_volume_sync = make_timeout_time_ms(180);
    return true;
}

static bool process_volume_sync(bool source_5v, bool bus_high) {
    if (!pending_volume_sync && !pending_mute_sync && !relative_volume_sync_active) {
        return false;
    }

    if (absolute_time_diff_us(get_absolute_time(), next_volume_sync) > 0) {
        return false;
    }

    if (pending_volume_sync) {
        const uint8_t desired_volume = pending_volume;
        pending_volume_sync = false;

        bool ack = false;
        if (absolute_volume_supported) {
            ack = send_set_audio_volume_level(desired_volume);
            printf("arc: set-audio-volume-level volume=%u ack=%s src5v=%d idle=%d\n",
                   desired_volume,
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
            if (!ack) {
                absolute_volume_supported = false;
                start_relative_volume_sync(desired_volume);
            } else {
                cec_audio_volume = desired_volume;
                cec_audio_volume_known = true;
                if (!audio_is_streaming()) {
                    absolute_volume_verify_pending = true;
                    absolute_volume_verify_target = desired_volume;
                    send_give_audio_status();
                }
            }
        } else {
            ack = start_relative_volume_sync(desired_volume);
            if (!ack && !cec_audio_volume_known) {
                pending_volume = desired_volume;
                pending_volume_sync = true;
                const bool status_ack = send_give_audio_status();
                if (!audio_is_streaming()) {
                    printf("arc: defer volume sync until audio-status ack=%s src5v=%d idle=%d\n",
                           status_ack ? "yes" : "no",
                           source_5v,
                           bus_high);
                }
                next_volume_sync = make_timeout_time_ms(500);
                return true;
            }
        }

        next_volume_sync = make_timeout_time_ms(150);
        return true;
    }

    if (continue_relative_volume_sync(source_5v, bus_high)) {
        return true;
    }

    if (pending_mute_sync) {
        if (!cec_audio_mute_known) {
            const bool ack = send_give_audio_status();
            printf("arc: defer mute sync until audio-status ack=%s src5v=%d idle=%d\n",
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
            next_volume_sync = make_timeout_time_ms(500);
            return true;
        }

        const bool desired_mute = pending_mute;
        pending_mute_sync = false;
        if (cec_audio_muted == desired_mute) {
            return false;
        }

        const bool ack = send_user_control_tap(0x43);
        printf("arc: user-control mute desired=%s ack=%s src5v=%d idle=%d\n",
               desired_mute ? "on" : "off",
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        if (ack) {
            cec_audio_muted = desired_mute;
            cec_audio_mute_known = true;
        }
        next_volume_sync = make_timeout_time_ms(150);
        return true;
    }

    return false;
}

static bool probe_device_info(bool source_5v, bool bus_high, const char *prefix) {
    if (device_info_probe_done) {
        return false;
    }

    bool ack = false;
    switch (device_info_probe_step) {
    case 0:
        ack = request_soundbar_osd_name(source_5v, bus_high, prefix);
        break;
    case 1:
        ack = send_give_device_vendor_id();
        printf("arc: %sgive-device-vendor-id ack=%s src5v=%d idle=%d\n",
               prefix,
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        break;
    case 2:
        ack = send_get_cec_version();
        printf("arc: %sget-cec-version ack=%s src5v=%d idle=%d\n",
               prefix,
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        break;
    case 3:
        ack = send_request_current_latency();
        printf("arc: %srequest-current-latency 0000 ack=%s src5v=%d idle=%d\n",
               prefix,
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        break;
    default:
        device_info_probe_done = true;
        return false;
    }

    device_info_probe_step++;
    if (device_info_probe_step >= 4) {
        device_info_probe_done = true;
    }
    next_probe = make_timeout_time_ms(300);
    return true;
}

static void probe_arc_while_streaming(bool source_5v, bool bus_high) {
    if (system_audio_mode && arc_initiated) {
        if (!probe_device_info(source_5v, bus_high, "streaming ")) {
            next_probe = make_timeout_time_ms(10000);
        }
        return;
    }

    switch (streaming_probe_step) {
    case 0:
        if (!system_audio_mode) {
            const bool ack = send_system_audio_mode_request();
            printf("arc: streaming system-audio-mode-request 0000 ack=%s src5v=%d idle=%d\n",
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
        }
        break;
    case 1:
        if (!arc_initiated) {
            const bool ack = send_request_arc_initiation();
            printf("arc: streaming request-arc-initiation ack=%s src5v=%d idle=%d\n",
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
        }
        break;
    default:
        if (!system_audio_mode) {
            const bool ack = send_give_system_audio_mode_status();
            printf("arc: streaming give-system-audio-mode-status ack=%s src5v=%d idle=%d\n",
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
        }
        break;
    }

    streaming_probe_step = (streaming_probe_step + 1) % 4;
    next_probe = make_timeout_time_ms(2000);
}

static void handle_frame(const cec_frame_t *frame, bool streaming, bool allow_tx) {
    if (should_print_frame(frame, streaming)) {
        print_frame(frame);
    }

    if (frame->len < 2) {
        return;
    }

    const uint8_t initiator = frame->bytes[0] >> 4;
    const uint8_t destination = frame->bytes[0] & 0x0f;
    const uint8_t opcode = frame->bytes[1];
    note_logical_address_seen(initiator);

    if (opcode == 0x7a && frame->len >= 3) {
        update_cec_audio_status(frame->bytes[2]);
    }

    if (opcode == 0x47 && frame->len >= 3 &&
        initiator == CEC_LOGICAL_AUDIO_SYSTEM && destination == CEC_LOGICAL_TV) {
        capture_soundbar_osd_name(frame);
    }

    if (opcode == 0x00 && frame->len >= 4 && frame->bytes[2] == 0x73) {
        absolute_volume_supported = false;
        printf("arc: absolute volume disabled by feature-abort reason=%s(0x%02x)\n",
               feature_abort_reason_name(frame->bytes[3]),
               frame->bytes[3]);
        if (absolute_volume_verify_pending) {
            absolute_volume_verify_pending = false;
            start_relative_volume_sync(absolute_volume_verify_target);
        }
    }

    if (opcode == 0x00 && frame->len >= 4 && frame->bytes[2] == 0xa4) {
        if (sad_query_state == SAD_QUERY_REQUESTED) {
            sad_query_state = SAD_QUERY_UNSUPPORTED;
            reset_sad_caps();
            sad_pcm_rates_16 = PICOARC_AUDIO_RATE_BIT_48K;
            reset_sad_extra_queries();
            printf("arc: SAD query aborted reason=%s(0x%02x); defaulting to PCM 48k/16\n",
                   feature_abort_reason_name(frame->bytes[3]),
                   frame->bytes[3]);
        } else if (sad_extra_query_requested) {
            printf("arc: extra SAD query %s aborted reason=%s(0x%02x)\n",
                   sad_extra_queries[sad_extra_query_index].label,
                   feature_abort_reason_name(frame->bytes[3]),
                   frame->bytes[3]);
            sad_extra_query_requested = false;
            sad_extra_query_index++;
        }
    }

    if ((opcode == 0x72 || opcode == 0x7e) && frame->len >= 3) {
        system_audio_mode = frame->bytes[2] != 0;
        printf("arc: system-audio-mode %s\n", system_audio_mode ? "on" : "off");
    }

    if (opcode == 0xa3 && frame->len >= 2 && initiator == CEC_LOGICAL_AUDIO_SYSTEM) {
        if (sad_query_state == SAD_QUERY_REQUESTED) {
            parse_short_audio_descriptors(frame, true);
            reset_sad_extra_queries();
        } else {
            parse_short_audio_descriptors(frame, false);
            if (sad_extra_query_requested) {
                sad_extra_query_requested = false;
                sad_extra_query_index++;
            }
        }
    }

    if (destination == CEC_LOGICAL_BROADCAST || destination != CEC_LOGICAL_TV) {
        return;
    }

    switch (opcode) {
    case 0x83:
        if (allow_tx) {
            printf("arc: reply report-physical-address 0000 tv ack=%s\n",
                   send_report_physical_address(CEC_LOGICAL_TV, tv_physical_address, 0x00) ? "yes" : "no");
        }
        break;
    case 0x8c:
        if (allow_tx) {
            printf("arc: reply device-vendor-id ack=%s\n",
                   send_device_vendor_id(CEC_LOGICAL_TV) ? "yes" : "no");
        }
        break;
    case 0x8f:
        if (allow_tx) {
            printf("arc: reply report-power-status:on ack=%s\n",
                   send_power_status_on(CEC_LOGICAL_TV, initiator) ? "yes" : "no");
        }
        break;
    case 0x9f:
        if (allow_tx) {
            printf("arc: reply cec-version:2.0 ack=%s\n",
                   send_cec_version(CEC_LOGICAL_TV, initiator) ? "yes" : "no");
        }
        break;
    case 0x46:
        if (allow_tx) {
            printf("arc: reply set-osd-name:PicoARC ack=%s\n",
                   send_set_osd_name(CEC_LOGICAL_TV, initiator, "PicoARC") ? "yes" : "no");
        }
        break;
    case 0x00:
    case 0x47:
    case 0x72:
    case 0x7a:
    case 0x7e:
    case 0x90:
    case 0x9e:
    case 0xa3:
        break;
    case 0xc1:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM && !arc_initiated) {
            arc_initiated = true;
            printf("arc: initiated by soundbar report\n");
        }
        break;
    case 0xc2:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM) {
            arc_initiated = false;
            sad_query_state = SAD_QUERY_NOT_STARTED;
            reset_sad_caps();
            reset_sad_extra_queries();
            printf("arc: terminated by soundbar report\n");
        }
        break;
    case 0xc4:
    case 0xc5:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM) {
            arc_initiated = false;
            sad_query_state = SAD_QUERY_NOT_STARTED;
            reset_sad_caps();
            reset_sad_extra_queries();
            printf("arc: termination requested by soundbar\n");
        }
        break;
    case 0xc0:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM) {
            arc_initiated = true;
            printf("arc: initiate-arc from soundbar; report-arc-initiated ack=%s\n",
                   send_report_arc_initiated() ? "yes" : "no");
        }
        break;
    case 0xc3:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM && allow_tx) {
            printf("arc: request from soundbar; initiate-arc ack=%s\n",
                   send_initiate_arc() ? "yes" : "no");
        }
        break;
    default:
        if (allow_tx) {
            printf("arc: feature-abort opcode=0x%02x ack=%s\n",
                   opcode,
                   send_feature_abort(CEC_LOGICAL_TV, initiator, opcode, 0x00) ? "yes" : "no");
        }
        break;
    }
}

void arc_init(unsigned int cec_pin, unsigned int hdmi_5v_gpio) {
    hdmi_5v_pin = hdmi_5v_gpio;
    arc_initiated = false;
    system_audio_mode = false;
    last_streaming = false;
    last_source_5v = false;
    probe_step = 0;
    streaming_probe_step = 0;
    reset_logical_address_scan();
    next_probe = make_timeout_time_ms(1000);
    next_volume_sync = make_timeout_time_ms(0);
    next_stream_audio_log_us = 0;
    suppressed_stream_audio_logs = 0;
    cec_audio_volume_known = false;
    cec_audio_mute_known = false;
    cec_audio_volume = 0;
    cec_audio_muted = false;
    // Many ARC devices ACK <Set Audio Volume Level> but ignore it. Start with
    // relative key control; feature-abort handling can still keep us there.
    absolute_volume_supported = false;
    absolute_volume_verify_pending = false;
    absolute_volume_verify_target = 0;
    pending_volume_sync = false;
    pending_volume = 0;
    pending_mute_sync = false;
    pending_mute = false;
    relative_volume_sync_active = false;
    relative_volume_target = 0;
    relative_volume_key_held = false;
    relative_volume_held_key = 0;
    sad_query_state = SAD_QUERY_NOT_STARTED;
    sad_query_deadline = make_timeout_time_ms(0);
    reset_sad_caps();
    reset_sad_extra_queries();
    reset_device_info_probe();

    gpio_init(hdmi_5v_pin);
    gpio_set_dir(hdmi_5v_pin, GPIO_IN);
    gpio_disable_pulls(hdmi_5v_pin);

    cec_init(cec_pin);
    cec_set_logical_address(CEC_LOGICAL_TV);
    cec_passive_reset();

    printf("arc: CEC on GP%u, HDMI +5V sense on GP%u\n", cec_pin, hdmi_5v_pin);
    printf("arc: TV ARC endpoint mode\n");
}

void arc_task(void) {
    cec_frame_t frame;
    const bool streaming = audio_is_streaming();
    unsigned int cec_frames = 0;

    while (cec_frames < max_cec_frames_per_task && cec_receive_frame_passive(&frame)) {
        handle_frame(&frame, streaming, !streaming);
        cec_frames++;
    }

    const bool source_5v = gpio_get(hdmi_5v_pin);
    const bool bus_high = cec_bus_is_high();

    if (source_5v != last_source_5v) {
        last_source_5v = source_5v;
        probe_step = 0;
        streaming_probe_step = 0;
        reset_logical_address_scan();
        ddc_cec_wait_logged = false;
        next_probe = make_timeout_time_ms(source_5v ? 250 : 2000);
        if (!source_5v) {
            arc_initiated = false;
            system_audio_mode = false;
            relative_volume_sync_active = false;
            relative_volume_key_held = false;
            sad_query_state = SAD_QUERY_NOT_STARTED;
            reset_sad_caps();
            reset_sad_extra_queries();
            reset_device_info_probe();
        } else {
#if PICOARC_DDC_EDID_ENABLE
            ddc_edid_note_hotplug();
#endif
        }
        printf("arc: HDMI +5V %s\n", source_5v ? "present" : "lost");
    }

    if (streaming != last_streaming) {
        last_streaming = streaming;
        streaming_probe_step = 0;
    }

    if (!source_5v) {
        return;
    }

    update_soundbar_name_timeout();

#if PICOARC_DDC_EDID_ENABLE
    if (!ddc_edid_ready_for_cec()) {
        if (!ddc_cec_wait_logged) {
            ddc_cec_wait_logged = true;
            printf("arc: waiting for DDC EDID read/settle before CEC scan\n");
        }
        return;
    }
    if (ddc_cec_wait_logged) {
        ddc_cec_wait_logged = false;
        printf("arc: DDC settled; starting CEC scan\n");
    }
#endif

    if (service_logical_address_scan(source_5v, bus_high)) {
        return;
    }

    if (process_volume_sync(source_5v, bus_high)) {
        return;
    }

    if (arc_initiated && sad_query_state == SAD_QUERY_REQUESTED &&
        absolute_time_diff_us(get_absolute_time(), sad_query_deadline) <= 0) {
        sad_query_state = SAD_QUERY_TIMEOUT;
        reset_sad_caps();
        reset_sad_extra_queries();
        sad_pcm_rates_16 = PICOARC_AUDIO_RATE_BIT_48K;
        printf("arc: SAD query timed out; defaulting to PCM 48k/16\n");
    }

    if (arc_initiated && sad_query_state == SAD_QUERY_NOT_STARTED) {
        const bool ack = send_request_short_audio_descriptor();
        sad_query_state = SAD_QUERY_REQUESTED;
        sad_query_deadline = make_timeout_time_ms(1500);
        printf("arc: request-short-audio-descriptor LPCM/AC3/DTS ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no",
               source_5v,
               bus_high);
        return;
    }

#if !PICOARC_DEBUG_USB
    if (arc_initiated && sad_query_finished() &&
        !soundbar_name_ready && !soundbar_name_probe_started) {
        request_soundbar_osd_name(source_5v, bus_high, "pre-usb ");
        return;
    }
#endif

#if PICOARC_CEC_QUERY_EXTRA_SADS
    if (arc_initiated && sad_query_state == SAD_QUERY_REPORTED) {
        if (sad_extra_query_requested &&
            absolute_time_diff_us(get_absolute_time(), sad_extra_query_deadline) <= 0) {
            printf("arc: extra SAD query %s timed out\n",
                   sad_extra_queries[sad_extra_query_index].label);
            sad_extra_query_requested = false;
            sad_extra_query_index++;
        }

        if (!sad_extra_query_requested &&
            sad_extra_query_index < sizeof(sad_extra_queries) / sizeof(sad_extra_queries[0])) {
            const sad_query_batch_t *query = &sad_extra_queries[sad_extra_query_index];
            const bool ack = send_request_short_audio_descriptor_operands(query->operands, query->count);
            sad_extra_query_requested = true;
            sad_extra_query_deadline = make_timeout_time_ms(1200);
            printf("arc: extra request-short-audio-descriptor %s", query->label);
            for (uint8_t i = 0; i < query->count; i++) {
                printf(" %s", sad_request_operand_name(query->operands[i]));
            }
            printf(" ack=%s src5v=%d idle=%d\n",
                   ack ? "yes" : "no",
                   source_5v,
                   bus_high);
            return;
        }
    }
#endif

    if (absolute_time_diff_us(get_absolute_time(), next_probe) > 0) {
        return;
    }

    if (streaming) {
        probe_arc_while_streaming(source_5v, bus_high);
        return;
    }

    if (probe_device_info(source_5v, bus_high, "")) {
        return;
    }

    if (idle_probe_complete()) {
        return;
    }

    if (probe_step == 0) {
        const bool ack = cec_poll(CEC_LOGICAL_TV, CEC_LOGICAL_AUDIO_SYSTEM);
        printf("arc: audio-system poll ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (probe_step == 1) {
        const uint8_t give_power_status[] = {
            (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
            0x8f,
        };
        const bool ack = cec_send_with_retry(give_power_status, sizeof(give_power_status), 2);
        printf("arc: give-device-power-status ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (probe_step == 2) {
        const bool ack = send_report_physical_address(CEC_LOGICAL_TV, tv_physical_address, 0x00);
        printf("arc: report-physical-address 0000 tv ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (probe_step == 3) {
        const bool ack = send_give_system_audio_mode_status();
        printf("arc: give-system-audio-mode-status ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (probe_step == 4 && !system_audio_mode) {
        const bool ack = send_system_audio_mode_request();
        printf("arc: system-audio-mode-request 0000 ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (probe_step == 5) {
        const bool ack = send_give_audio_status();
        printf("arc: give-audio-status ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else if (!arc_initiated) {
        const bool ack = send_request_arc_initiation();
        printf("arc: request-arc-initiation ack=%s src5v=%d idle=%d\n",
               ack ? "yes" : "no", source_5v, bus_high);
    } else {
        printf("arc: initiated src5v=%d idle=%d\n", source_5v, bus_high);
    }

    probe_step = (probe_step + 1) % 7;
    next_probe = make_timeout_time_ms(2000);
}

bool arc_is_initiated(void) {
    return arc_initiated;
}

bool arc_hdmi_connected(void) {
    return last_source_5v;
}

bool arc_cec_scan_complete(void) {
    return logical_scan_done;
}

bool arc_cec_any_device_found(void) {
    return logical_scan_any_device_seen();
}

bool arc_cec_audio_system_found(void) {
    return logical_scan_audio_system_seen();
}

bool arc_system_audio_enabled(void) {
    return system_audio_mode;
}

bool arc_audio_caps_ready(void) {
    return sad_query_finished();
}

bool arc_ready_for_usb(void) {
    return arc_initiated && arc_audio_caps_ready() && soundbar_name_ready_for_usb();
}

static bool audio_format_supported(uint8_t alt, uint32_t sample_rate, bool log_rejection) {
    if (alt == PICOARC_AUDIO_ALT_ZERO) {
        return true;
    }

    if (!arc_initiated) {
        if (log_rejection) {
            printf("arc: reject audio alt=%u rate=%lu; ARC not initiated\n",
                   alt,
                   (unsigned long)sample_rate);
        }
        return false;
    }

    if (!sad_query_finished()) {
        if (log_rejection) {
            printf("arc: reject audio alt=%u rate=%lu; SAD query %s\n",
                   alt,
                   (unsigned long)sample_rate,
                   sad_query_state_name());
        }
        return false;
    }

    const uint8_t rate_bit = sample_rate_to_sad_bit(sample_rate);
    bool supported = false;
    switch (alt) {
    case PICOARC_AUDIO_ALT_PCM_16:
        supported = (sad_pcm_rates_16 & rate_bit) != 0;
        break;
    case PICOARC_AUDIO_ALT_PCM_20:
        supported = (sad_pcm_rates_20 & rate_bit) != 0;
        break;
    case PICOARC_AUDIO_ALT_PCM_24:
        supported = (sad_pcm_rates_24 & rate_bit) != 0;
        break;
    case PICOARC_AUDIO_ALT_IEC61937:
        supported = ((sad_ac3_rates | sad_dts_rates) & rate_bit) != 0;
        break;
    default:
        supported = false;
        break;
    }

    if (!supported && log_rejection) {
        printf("arc: reject audio alt=%u rate=%lu; caps pcm16=0x%02x pcm20=0x%02x pcm24=0x%02x ac3=0x%02x dts=0x%02x\n",
               alt,
               (unsigned long)sample_rate,
               sad_pcm_rates_16,
               sad_pcm_rates_20,
               sad_pcm_rates_24,
               sad_ac3_rates,
               sad_dts_rates);
    }

    return supported;
}

bool arc_audio_format_supported(uint8_t alt, uint32_t sample_rate) {
    return audio_format_supported(alt, sample_rate, true);
}

bool arc_audio_format_supported_quiet(uint8_t alt, uint32_t sample_rate) {
    return audio_format_supported(alt, sample_rate, false);
}

void arc_request_volume_sync(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }

    pending_volume = volume;
    pending_volume_sync = true;
    next_volume_sync = make_timeout_time_ms(audio_is_streaming() ?
                                            streaming_volume_sync_debounce_ms :
                                            0);
}

void arc_request_mute_sync(bool muted) {
    pending_mute = muted;
    pending_mute_sync = true;
    next_volume_sync = make_timeout_time_ms(0);
}
