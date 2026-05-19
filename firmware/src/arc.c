#include "arc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cec.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "usb_audio.h"

static unsigned int hdmi_5v_pin;
static bool arc_initiated;
static bool system_audio_mode;
static bool last_streaming;
static bool last_source_5v;
static unsigned int probe_step;
static unsigned int streaming_probe_step;
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

static const unsigned int max_cec_frames_per_task = 8;
static const uint16_t tv_physical_address = 0x0000;

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
            printf(" pa=%x.%x.%x.%x(0x%04x) type=%s(0x%02x)",
                   (physical_address >> 12) & 0x0f,
                   (physical_address >> 8) & 0x0f,
                   (physical_address >> 4) & 0x0f,
                   physical_address & 0x0f,
                   physical_address,
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
    sleep_ms(40);
    const bool release_ack = send_user_control_released();
    return press_ack && release_ack;
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

static void update_cec_audio_status(uint8_t status) {
    const bool muted = (status & 0x80) != 0;
    const uint8_t volume = status & 0x7f;

    cec_audio_muted = muted;
    cec_audio_mute_known = true;
    if (volume <= 100) {
        cec_audio_volume = volume;
        cec_audio_volume_known = true;
        usb_audio_set_cec_audio_status(volume, muted);
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
        usb_audio_set_cec_mute_status(muted);
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
        return false;
    }

    const uint8_t key = relative_volume_target > cec_audio_volume ? 0x41 : 0x42;
    const bool ack = send_user_control_tap(key);
    if (ack) {
        cec_audio_volume += key == 0x41 ? 1 : -1;
    }

    printf("arc: relative volume step key=%s now=%u target=%u ack=%s src5v=%d idle=%d\n",
           user_control_name(key),
           cec_audio_volume,
           relative_volume_target,
           ack ? "yes" : "no",
           source_5v,
           bus_high);

    send_give_audio_status();
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
                absolute_volume_verify_pending = true;
                absolute_volume_verify_target = desired_volume;
                send_give_audio_status();
            }
        } else {
            ack = start_relative_volume_sync(desired_volume);
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

static void probe_arc_while_streaming(bool source_5v, bool bus_high) {
    if (system_audio_mode && arc_initiated) {
        next_probe = make_timeout_time_ms(10000);
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

    if (opcode == 0x7a && frame->len >= 3) {
        update_cec_audio_status(frame->bytes[2]);
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

    if ((opcode == 0x72 || opcode == 0x7e) && frame->len >= 3) {
        system_audio_mode = frame->bytes[2] != 0;
        printf("arc: system-audio-mode %s\n", system_audio_mode ? "on" : "off");
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
    case 0x72:
    case 0x7a:
    case 0x7e:
    case 0x90:
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
            printf("arc: terminated by soundbar report\n");
        }
        break;
    case 0xc4:
    case 0xc5:
        if (initiator == CEC_LOGICAL_AUDIO_SYSTEM) {
            arc_initiated = false;
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
    next_probe = make_timeout_time_ms(1000);
    next_volume_sync = make_timeout_time_ms(0);
    next_stream_audio_log_us = 0;
    suppressed_stream_audio_logs = 0;
    cec_audio_volume_known = false;
    cec_audio_mute_known = false;
    cec_audio_volume = 0;
    cec_audio_muted = false;
    absolute_volume_supported = true;
    absolute_volume_verify_pending = false;
    absolute_volume_verify_target = 0;
    pending_volume_sync = false;
    pending_volume = 0;
    pending_mute_sync = false;
    pending_mute = false;
    relative_volume_sync_active = false;
    relative_volume_target = 0;

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
        next_probe = make_timeout_time_ms(source_5v ? 250 : 2000);
        if (!source_5v) {
            arc_initiated = false;
            system_audio_mode = false;
            relative_volume_sync_active = false;
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

    if (process_volume_sync(source_5v, bus_high)) {
        return;
    }

    if (absolute_time_diff_us(get_absolute_time(), next_probe) > 0) {
        return;
    }

    if (streaming) {
        probe_arc_while_streaming(source_5v, bus_high);
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

void arc_request_volume_sync(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }

    pending_volume = volume;
    pending_volume_sync = true;
    next_volume_sync = make_timeout_time_ms(0);
}

void arc_request_mute_sync(bool muted) {
    pending_mute = muted;
    pending_mute_sync = true;
    next_volume_sync = make_timeout_time_ms(0);
}
