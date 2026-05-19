#include "arc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cec.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "usb_audio.h"

static unsigned int hpd_pin;
static bool arc_initiated;
static bool system_audio_mode;
static bool last_streaming;
static unsigned int probe_step;
static unsigned int streaming_probe_step;
static absolute_time_t next_probe;
static uint64_t next_stream_audio_log_us;
static unsigned int suppressed_stream_audio_logs;

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
        0x05,
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

static void probe_arc_while_streaming(bool hpd, bool bus_high) {
    if (system_audio_mode && arc_initiated) {
        next_probe = make_timeout_time_ms(10000);
        return;
    }

    switch (streaming_probe_step) {
    case 0:
        if (!system_audio_mode) {
            const bool ack = send_system_audio_mode_request();
            printf("arc: streaming system-audio-mode-request 0000 ack=%s hpd=%d idle=%d\n",
                   ack ? "yes" : "no",
                   hpd,
                   bus_high);
        }
        break;
    case 1:
        if (!arc_initiated) {
            const bool ack = send_request_arc_initiation();
            printf("arc: streaming request-arc-initiation ack=%s hpd=%d idle=%d\n",
                   ack ? "yes" : "no",
                   hpd,
                   bus_high);
        }
        break;
    default:
        if (!system_audio_mode) {
            const bool ack = send_give_system_audio_mode_status();
            printf("arc: streaming give-system-audio-mode-status ack=%s hpd=%d idle=%d\n",
                   ack ? "yes" : "no",
                   hpd,
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
        const bool muted = (frame->bytes[2] & 0x80) != 0;
        const uint8_t volume = frame->bytes[2] & 0x7f;
        usb_audio_set_cec_audio_status(volume, muted);
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
            printf("arc: reply cec-version:1.4 ack=%s\n",
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

void arc_init(unsigned int cec_pin, unsigned int hpd_gpio) {
    hpd_pin = hpd_gpio;
    arc_initiated = false;
    system_audio_mode = false;
    last_streaming = false;
    probe_step = 0;
    streaming_probe_step = 0;
    next_probe = make_timeout_time_ms(1000);
    next_stream_audio_log_us = 0;
    suppressed_stream_audio_logs = 0;

    gpio_init(hpd_pin);
    gpio_set_dir(hpd_pin, GPIO_IN);

    cec_init(cec_pin);
    cec_set_logical_address(CEC_LOGICAL_TV);
    cec_passive_reset();

    printf("arc: CEC on GP%u, HPD on GP%u\n", cec_pin, hpd_pin);
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

    if (absolute_time_diff_us(get_absolute_time(), next_probe) > 0) {
        return;
    }

    const bool hpd = gpio_get(hpd_pin);
    const bool bus_high = cec_bus_is_high();

    if (streaming != last_streaming) {
        last_streaming = streaming;
        streaming_probe_step = 0;
    }

    if (streaming) {
        probe_arc_while_streaming(hpd, bus_high);
        return;
    }

    if (probe_step == 0) {
        const bool ack = cec_poll(CEC_LOGICAL_TV, CEC_LOGICAL_AUDIO_SYSTEM);
        printf("arc: audio-system poll ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (probe_step == 1) {
        const uint8_t give_power_status[] = {
            (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
            0x8f,
        };
        const bool ack = cec_send_with_retry(give_power_status, sizeof(give_power_status), 2);
        printf("arc: give-device-power-status ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (probe_step == 2) {
        const bool ack = send_report_physical_address(CEC_LOGICAL_TV, tv_physical_address, 0x00);
        printf("arc: report-physical-address 0000 tv ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (probe_step == 3) {
        const bool ack = send_give_system_audio_mode_status();
        printf("arc: give-system-audio-mode-status ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (probe_step == 4 && !system_audio_mode) {
        const bool ack = send_system_audio_mode_request();
        printf("arc: system-audio-mode-request 0000 ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (probe_step == 5) {
        const bool ack = send_give_audio_status();
        printf("arc: give-audio-status ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (!arc_initiated) {
        const bool ack = send_request_arc_initiation();
        printf("arc: request-arc-initiation ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else {
        printf("arc: initiated hpd=%d idle=%d\n", hpd, bus_high);
    }

    probe_step = (probe_step + 1) % 7;
    next_probe = make_timeout_time_ms(2000);
}

bool arc_is_initiated(void) {
    return arc_initiated;
}
