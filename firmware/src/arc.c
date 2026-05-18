#include "arc.h"

#include <stdbool.h>
#include <stdio.h>

#include "cec.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "spdif.h"
#include "usb_audio.h"

static unsigned int hpd_pin;
static bool arc_initiated;
static unsigned int probe_step;
static absolute_time_t next_probe;
static uint64_t next_stream_audio_log_us;
static unsigned int suppressed_stream_audio_logs;

static const char *opcode_name(uint8_t opcode) {
    switch (opcode) {
    case 0x36:
        return "standby";
    case 0x44:
        return "user-control-pressed";
    case 0x45:
        return "user-control-released";
    case 0x70:
        return "system-audio-mode-request";
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
    case 0x89:
        return "vendor-command";
    case 0x8c:
        return "give-device-vendor-id";
    case 0x8f:
        return "give-device-power-status";
    case 0x90:
        return "report-power-status";
    case 0x9f:
        return "get-cec-version";
    case 0xa0:
        return "cec-version";
    case 0xc0:
        return "initiated-arc";
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
    case 0x0a:
        return "setup-menu";
    case 0x0b:
        return "contents-menu";
    case 0x0c:
        return "favorite-menu";
    case 0x0d:
        return "exit";
    case 0x20:
        return "number-0";
    case 0x21:
        return "number-1";
    case 0x22:
        return "number-2";
    case 0x23:
        return "number-3";
    case 0x24:
        return "number-4";
    case 0x25:
        return "number-5";
    case 0x26:
        return "number-6";
    case 0x27:
        return "number-7";
    case 0x28:
        return "number-8";
    case 0x29:
        return "number-9";
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
    case 0x47:
        return "record";
    case 0x48:
        return "rewind";
    case 0x49:
        return "fast-forward";
    case 0x4a:
        return "eject";
    case 0x4b:
        return "forward";
    case 0x4c:
        return "backward";
    case 0x60:
        return "play-function";
    case 0x61:
        return "pause-play-function";
    case 0x62:
        return "record-function";
    case 0x63:
        return "pause-record-function";
    case 0x64:
        return "stop-function";
    case 0x65:
        return "mute-function";
    case 0x6b:
        return "power-toggle-function";
    case 0x6c:
        return "power-off-function";
    case 0x6d:
        return "power-on-function";
    default:
        return "unknown";
    }
}

static void print_frame(const cec_frame_t *frame) {
    if (frame->len == 0) {
        return;
    }

    const uint8_t header = frame->bytes[0];
    printf("cec: rx %x->%x", header >> 4, header & 0x0f);
    for (size_t i = 1; i < frame->len; i++) {
        printf(" %02x", frame->bytes[i]);
    }

    if (frame->len >= 2) {
        printf(" <%s>", opcode_name(frame->bytes[1]));
        if (frame->bytes[1] == 0x7a && frame->len >= 3) {
            const bool muted = (frame->bytes[2] & 0x80) != 0;
            const uint8_t volume = frame->bytes[2] & 0x7f;
            printf(" volume=%u muted=%s", volume, muted ? "yes" : "no");
        } else if (frame->bytes[1] == 0x44 && frame->len >= 3) {
            printf(" key=%s(0x%02x)", user_control_name(frame->bytes[2]), frame->bytes[2]);
        } else if (frame->bytes[1] == 0x72 && frame->len >= 3) {
            printf(" enabled=%s", frame->bytes[2] ? "yes" : "no");
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

static bool send_report_physical_address(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_BROADCAST,
        0x84,
        0x00,
        0x00,
        0x00,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_cec_version(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xa0,
        0x05,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool send_power_status_on(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0x90,
        0x00,
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

static bool send_report_arc_initiated(void) {
    const uint8_t msg[] = {
        (CEC_LOGICAL_TV << 4) | CEC_LOGICAL_AUDIO_SYSTEM,
        0xc1,
    };
    return cec_send_with_retry(msg, sizeof(msg), 2);
}

static bool audio_is_streaming(void) {
    return spdif_get_mode() == SPDIF_MODE_USB_AUDIO;
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

    if (destination != CEC_LOGICAL_TV || initiator != CEC_LOGICAL_AUDIO_SYSTEM) {
        return;
    }

    switch (opcode) {
    case 0x83:
        if (allow_tx) {
            printf("arc: reply report-physical-address ack=%s\n",
                   send_report_physical_address() ? "yes" : "no");
        }
        break;
    case 0x8f:
        if (allow_tx) {
            printf("arc: reply report-power-status:on ack=%s\n",
                   send_power_status_on() ? "yes" : "no");
        }
        break;
    case 0x9f:
        if (allow_tx) {
            printf("arc: reply cec-version:1.4 ack=%s\n",
                   send_cec_version() ? "yes" : "no");
        }
        break;
    case 0xc0:
        if (arc_initiated) {
            break;
        }
        arc_initiated = true;
        if (allow_tx) {
            printf("arc: initiated by soundbar; report-arc-initiated ack=%s\n",
                   send_report_arc_initiated() ? "yes" : "no");
        }
        break;
    case 0xc2:
        arc_initiated = false;
        printf("arc: terminated by soundbar report\n");
        break;
    case 0xc5:
        arc_initiated = false;
        printf("arc: terminate requested by soundbar\n");
        break;
    default:
        break;
    }
}

void arc_init(unsigned int cec_pin, unsigned int hpd_gpio) {
    hpd_pin = hpd_gpio;
    arc_initiated = false;
    probe_step = 0;
    next_probe = make_timeout_time_ms(1000);

    gpio_init(hpd_pin);
    gpio_set_dir(hpd_pin, GPIO_IN);

    cec_init(cec_pin);
    cec_set_logical_address(CEC_LOGICAL_TV);
    cec_passive_reset();

    printf("arc: CEC on GP%u, HPD on GP%u\n", cec_pin, hpd_pin);
}

void arc_task(void) {
    cec_frame_t frame;
    const bool streaming = audio_is_streaming();

    while (cec_receive_frame_passive(&frame)) {
        handle_frame(&frame, streaming, !streaming);
    }

    if (streaming) {
        next_probe = make_timeout_time_ms(2000);
        return;
    }

    if (absolute_time_diff_us(get_absolute_time(), next_probe) > 0) {
        return;
    }

    const bool hpd = gpio_get(hpd_pin);
    const bool bus_high = cec_bus_is_high();

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
        const bool ack = send_report_physical_address();
        printf("arc: report-physical-address 0000 tv ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else if (!arc_initiated) {
        const bool ack = send_request_arc_initiation();
        printf("arc: request-arc-initiation ack=%s hpd=%d idle=%d\n",
               ack ? "yes" : "no", hpd, bus_high);
    } else {
        printf("arc: initiated hpd=%d idle=%d\n", hpd, bus_high);
    }

    probe_step = (probe_step + 1) % 4;
    next_probe = make_timeout_time_ms(2000);
}

bool arc_is_initiated(void) {
    return arc_initiated;
}
