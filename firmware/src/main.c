#include <stdio.h>

#include "arc.h"
#include "cec.h"
#include "picoarc_log.h"
#include "pico/stdlib.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

#define SPDIF_PIN 2
#define CEC_PIN 3
#define HDMI_5V_PIN 4

typedef enum {
    ADAPTER_STATE_BOOT,
    ADAPTER_STATE_WAIT_HDMI,
    ADAPTER_STATE_CEC_DISCOVER,
    ADAPTER_STATE_ARC_INIT,
    ADAPTER_STATE_QUERY_CAPS,
    ADAPTER_STATE_USB_ATTACH,
    ADAPTER_STATE_USB_IDLE,
    ADAPTER_STATE_USB_STREAMING,
    ADAPTER_STATE_USB_DETACH,
#if PICOARC_DEBUG_USB
    ADAPTER_STATE_DEBUG_USB,
#endif
} adapter_state_t;

static void cec_yield_pump(void) {
    tud_task();
    usb_audio_task();
}

static const char *adapter_state_name(adapter_state_t state) {
    switch (state) {
    case ADAPTER_STATE_BOOT:
        return "boot";
    case ADAPTER_STATE_WAIT_HDMI:
        return "wait-hdmi";
    case ADAPTER_STATE_CEC_DISCOVER:
        return "cec-discover";
    case ADAPTER_STATE_ARC_INIT:
        return "arc-init";
    case ADAPTER_STATE_QUERY_CAPS:
        return "query-caps";
    case ADAPTER_STATE_USB_ATTACH:
        return "usb-attach";
    case ADAPTER_STATE_USB_IDLE:
        return "usb-idle";
    case ADAPTER_STATE_USB_STREAMING:
        return "usb-streaming";
    case ADAPTER_STATE_USB_DETACH:
        return "usb-detach";
#if PICOARC_DEBUG_USB
    case ADAPTER_STATE_DEBUG_USB:
        return "debug-usb";
#endif
    default:
        return "unknown";
    }
}

static void adapter_set_state(adapter_state_t *state, adapter_state_t next) {
    if (*state == next) {
        return;
    }

    printf("adapter: %s -> %s\n", adapter_state_name(*state), adapter_state_name(next));
    *state = next;
}

static void adapter_task(adapter_state_t *state, bool *usb_attached) {
#if PICOARC_DEBUG_USB
    if (*state == ADAPTER_STATE_DEBUG_USB) {
        if (usb_audio_is_streaming()) {
            usb_audio_task();
        }
        return;
    }
#endif

    const bool hdmi_connected = arc_hdmi_connected();
    const bool arc_initiated = arc_is_initiated();
    const bool caps_ready = arc_audio_caps_ready();
    const bool usb_ready = arc_ready_for_usb();

    if (*state >= ADAPTER_STATE_USB_ATTACH && *state <= ADAPTER_STATE_USB_STREAMING && !usb_ready) {
        adapter_set_state(state, ADAPTER_STATE_USB_DETACH);
    }

    switch (*state) {
    case ADAPTER_STATE_BOOT:
        adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        break;

    case ADAPTER_STATE_WAIT_HDMI:
        if (hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_CEC_DISCOVER);
        }
        break;

    case ADAPTER_STATE_CEC_DISCOVER:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (arc_system_audio_enabled() || arc_initiated) {
            adapter_set_state(state, ADAPTER_STATE_ARC_INIT);
        }
        break;

    case ADAPTER_STATE_ARC_INIT:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (arc_initiated) {
            adapter_set_state(state, ADAPTER_STATE_QUERY_CAPS);
        }
        break;

    case ADAPTER_STATE_QUERY_CAPS:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (usb_ready) {
            adapter_set_state(state, ADAPTER_STATE_USB_ATTACH);
        } else if (!arc_initiated) {
            adapter_set_state(state, ADAPTER_STATE_ARC_INIT);
        } else if (!caps_ready) {
            break;
        }
        break;

    case ADAPTER_STATE_USB_ATTACH:
        if (!*usb_attached) {
            tud_connect();
            *usb_attached = true;
            printf("adapter: USB audio attached to host\n");
        }
        adapter_set_state(state, ADAPTER_STATE_USB_IDLE);
        break;

    case ADAPTER_STATE_USB_IDLE:
        usb_audio_task();
        if (usb_audio_is_streaming()) {
            adapter_set_state(state, ADAPTER_STATE_USB_STREAMING);
        }
        break;

    case ADAPTER_STATE_USB_STREAMING:
        usb_audio_task();
        if (!usb_audio_is_streaming()) {
            adapter_set_state(state, ADAPTER_STATE_USB_IDLE);
        }
        break;

    case ADAPTER_STATE_USB_DETACH:
        usb_audio_stop_streaming();
        if (*usb_attached) {
            tud_disconnect();
            *usb_attached = false;
            printf("adapter: USB audio detached from host\n");
        }
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (!arc_initiated) {
            adapter_set_state(state, ADAPTER_STATE_ARC_INIT);
        } else {
            adapter_set_state(state, ADAPTER_STATE_QUERY_CAPS);
        }
        break;

#if PICOARC_DEBUG_USB
    case ADAPTER_STATE_DEBUG_USB:
        break;
#endif
    }
}

int main(void) {
    tusb_init();
#if !PICOARC_DEBUG_USB
    tud_disconnect();
#endif
#if PICOARC_LOGGING
    stdio_init_all();
#endif

    // Bring up the audio/CEC hardware before pumping tud_task. The host can
    // issue class-specific audio control transfers as soon as enumeration
    // completes, so the S/PDIF path must be ready before USB traffic is served.
    spdif_start(SPDIF_PIN);
    spdif_set_mode(SPDIF_MODE_SILENCE);
    arc_init(CEC_PIN, HDMI_5V_PIN);
    cec_set_yield(cec_yield_pump);

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);

    printf("\nPicoARC bring-up\n");

    printf("spdif: GP%d 48k stereo %s\n", SPDIF_PIN, spdif_mode_name(spdif_get_mode()));
#if PICOARC_DEBUG_USB
    printf("adapter: debug USB stays online; audio streaming remains ARC/SAD gated\n");
#else
    printf("adapter: release USB waits for HDMI ARC capabilities before enumeration\n");
#endif

    absolute_time_t next_blink = make_timeout_time_ms(250);
    bool led_on = false;
    bool usb_attached = PICOARC_DEBUG_USB;
    adapter_state_t adapter_state =
#if PICOARC_DEBUG_USB
        ADAPTER_STATE_DEBUG_USB;
#else
        ADAPTER_STATE_BOOT;
#endif

    while (true) {
        tud_task();
        arc_task();
        adapter_task(&adapter_state, &usb_attached);

        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            led_on = !led_on;
            gpio_put(led_pin, led_on);
            next_blink = make_timeout_time_ms(250);
        }

        tight_loop_contents();
    }
}
