#include <stdio.h>

#include "arc.h"
#include "cec.h"
#include "hardware/pwm.h"
#include "picoarc_config.h"
#include "picoarc_log.h"
#include "pico/stdlib.h"
#include "spdif.h"
#include "tusb.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

#define SPDIF_PIN 2
#define CEC_PIN 3
#define HDMI_5V_PIN 4
#define STATUS_LED_WAIT_HDMI_BLINK_MS 1000
#define STATUS_LED_WAIT_USB_BREATHE_MS 2400
#define STATUS_LED_PWM_WRAP 255

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

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_WAIT_HDMI,
    STATUS_LED_WAIT_USB,
} status_led_mode_t;

static void cec_yield_pump(void) {
    tud_task();
    usb_audio_task();
}

static spdif_mode_t idle_spdif_mode(void) {
#if PICOARC_IDLE_AUDIO_KEEPALIVE
    return SPDIF_MODE_SILENCE;
#else
    return SPDIF_MODE_OFF;
#endif
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

static void status_led_set_gpio(uint led_pin, bool *led_on, bool on) {
    if (*led_on == on) {
        return;
    }

    *led_on = on;
    gpio_put(led_pin, on);
}

static void status_led_enable_pwm(uint led_pin) {
    const uint slice = pwm_gpio_to_slice_num(led_pin);
    const uint channel = pwm_gpio_to_channel(led_pin);

    gpio_set_function(led_pin, GPIO_FUNC_PWM);
    pwm_set_wrap(slice, STATUS_LED_PWM_WRAP);
    pwm_set_chan_level(slice, channel, 0);
    pwm_set_enabled(slice, true);
}

static void status_led_disable_pwm(uint led_pin) {
    const uint slice = pwm_gpio_to_slice_num(led_pin);
    const uint channel = pwm_gpio_to_channel(led_pin);

    pwm_set_chan_level(slice, channel, 0);
    pwm_set_enabled(slice, false);
    gpio_set_function(led_pin, GPIO_FUNC_SIO);
    gpio_set_dir(led_pin, GPIO_OUT);
    gpio_put(led_pin, false);
}

static uint16_t status_led_breath_level(uint32_t elapsed_ms) {
    const uint32_t phase_ms = elapsed_ms % STATUS_LED_WAIT_USB_BREATHE_MS;
    const uint32_t half_ms = STATUS_LED_WAIT_USB_BREATHE_MS / 2;
    const uint32_t ramp_ms = phase_ms < half_ms ? phase_ms : STATUS_LED_WAIT_USB_BREATHE_MS - phase_ms;
    const uint32_t linear = (ramp_ms * STATUS_LED_PWM_WRAP) / half_ms;

    return (uint16_t)((linear * linear) / STATUS_LED_PWM_WRAP);
}

static void status_led_update_breath(uint led_pin, uint32_t started_ms) {
    const uint slice = pwm_gpio_to_slice_num(led_pin);
    const uint channel = pwm_gpio_to_channel(led_pin);
    const uint32_t elapsed_ms = to_ms_since_boot(get_absolute_time()) - started_ms;

    pwm_set_chan_level(slice, channel, status_led_breath_level(elapsed_ms));
}

static void status_led_task(uint led_pin,
                            status_led_mode_t mode,
                            status_led_mode_t *active_mode,
                            bool *led_on,
                            absolute_time_t *next_transition,
                            uint32_t *breath_started_ms) {
    if (*active_mode != mode) {
        if (*active_mode == STATUS_LED_WAIT_USB) {
            status_led_disable_pwm(led_pin);
            *led_on = false;
        }

        *active_mode = mode;

        if (mode == STATUS_LED_WAIT_USB) {
            *breath_started_ms = to_ms_since_boot(get_absolute_time());
            status_led_enable_pwm(led_pin);
            status_led_update_breath(led_pin, *breath_started_ms);
        } else {
            status_led_set_gpio(led_pin, led_on, mode == STATUS_LED_WAIT_HDMI);
            *next_transition = make_timeout_time_ms(STATUS_LED_WAIT_HDMI_BLINK_MS);
        }
        return;
    }

    if (mode == STATUS_LED_OFF) {
        if (*led_on) {
            status_led_set_gpio(led_pin, led_on, false);
        }
        return;
    }

    if (mode == STATUS_LED_WAIT_USB) {
        status_led_update_breath(led_pin, *breath_started_ms);
        return;
    }

    if (absolute_time_diff_us(get_absolute_time(), *next_transition) <= 0) {
        status_led_set_gpio(led_pin, led_on, !*led_on);
        *next_transition = make_timeout_time_ms(STATUS_LED_WAIT_HDMI_BLINK_MS);
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
    spdif_set_mode(idle_spdif_mode());
    arc_init(CEC_PIN, HDMI_5V_PIN);
    cec_set_yield(cec_yield_pump);

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);
    gpio_put(led_pin, false);

    printf("\nPicoARC bring-up\n");

    printf("spdif: GP%d 48k stereo %s\n", SPDIF_PIN, spdif_mode_name(spdif_get_mode()));
    printf("adapter: idle audio policy=%s\n", PICOARC_IDLE_AUDIO_POLICY);
#if PICOARC_DEBUG_USB
    printf("adapter: debug USB stays online; audio streaming remains ARC/SAD gated\n");
#else
    printf("adapter: release USB waits for HDMI ARC capabilities and OSD name before enumeration\n");
#endif

    absolute_time_t next_led_transition = make_timeout_time_ms(STATUS_LED_WAIT_HDMI_BLINK_MS);
    uint32_t led_breath_started_ms = 0;
    bool led_on = false;
    status_led_mode_t led_mode = STATUS_LED_OFF;
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

        const bool hdmi_connected = arc_hdmi_connected();
        const bool usb_enumerated = usb_attached && tud_mounted();
        const status_led_mode_t next_led_mode =
            !hdmi_connected ? STATUS_LED_WAIT_HDMI : (!usb_enumerated ? STATUS_LED_WAIT_USB : STATUS_LED_OFF);
        status_led_task(led_pin, next_led_mode, &led_mode, &led_on, &next_led_transition, &led_breath_started_ms);

        tight_loop_contents();
    }
}
