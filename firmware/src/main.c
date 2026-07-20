#include <stdio.h>

#include "arc.h"
#include "cec.h"
#include "ddc_edid.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include "picoarc_config.h"
#include "picoarc_log.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "realtime.h"

#define STATUS_LED_PWM_WRAP 255

typedef enum {
    ADAPTER_STATE_BOOT,
    ADAPTER_STATE_WAIT_HDMI,
    ADAPTER_STATE_CEC_SCAN,
    ADAPTER_STATE_CEC_DISCOVER,
    ADAPTER_STATE_ARC_INIT,
    ADAPTER_STATE_QUERY_CAPS,
    ADAPTER_STATE_USB_ATTACH,
    ADAPTER_STATE_USB_IDLE,
    ADAPTER_STATE_USB_STREAMING,
    ADAPTER_STATE_USB_DETACH,
    ADAPTER_STATE_USB_RECOVERY,
#if PICOARC_DEBUG_USB
    ADAPTER_STATE_DEBUG_USB,
#endif
} adapter_state_t;

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_WAIT_HDMI,
    STATUS_LED_WAIT_USB,
} status_led_mode_t;

static absolute_time_t usb_recovery_reconnect_at;

static void drain_realtime_requests(void) {
    uint8_t volume;
    bool muted;

    if (realtime_take_bootsel_reset_request()) {
        rom_reset_usb_boot_extra(
            -1, PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK, false);
    }
    if (realtime_take_volume_request(&volume)) {
        arc_request_volume_sync(volume);
    }
    if (realtime_take_mute_request(&muted)) {
        arc_request_mute_sync(muted);
    }
}

static void cec_yield_pump(void) {
    drain_realtime_requests();
}

#if PICOARC_HPD_GATE_ENABLE
static void hpd_gate_hold(unsigned int gate_pin) {
    gpio_init(gate_pin);
    gpio_put(gate_pin, true);
    gpio_set_dir(gate_pin, GPIO_OUT);
    printf("hpd: holding low through GP%u gate\n", gate_pin);
}

static void hpd_gate_release(unsigned int gate_pin) {
    gpio_put(gate_pin, false);
    printf("hpd: released GP%u gate\n", gate_pin);
}
#endif

static const char *adapter_state_name(adapter_state_t state) {
    switch (state) {
    case ADAPTER_STATE_BOOT:
        return "boot";
    case ADAPTER_STATE_WAIT_HDMI:
        return "wait-hdmi";
    case ADAPTER_STATE_CEC_SCAN:
        return "cec-scan";
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
    case ADAPTER_STATE_USB_RECOVERY:
        return "usb-recovery";
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

static void adapter_task(adapter_state_t *state, bool *usb_requested) {
    if (realtime_take_usb_recovery_request()) {
        if (*usb_requested) {
            realtime_set_usb_attached(false);
            *usb_requested = false;
        }
        usb_recovery_reconnect_at = make_timeout_time_ms(PICOARC_USB_RECOVERY_DISCONNECT_MS);
        printf("adapter: forcing USB re-enumeration after stalled audio stream\n");
        adapter_set_state(state, ADAPTER_STATE_USB_RECOVERY);
    }

#if PICOARC_DEBUG_USB
    if (*state == ADAPTER_STATE_DEBUG_USB) {
        return;
    }
#endif

    const bool hdmi_connected = arc_hdmi_connected();
    const bool audio_system_found = arc_cec_audio_system_found();
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
            adapter_set_state(state, ADAPTER_STATE_CEC_SCAN);
        }
        break;

    case ADAPTER_STATE_CEC_SCAN:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (audio_system_found) {
            adapter_set_state(state, ADAPTER_STATE_CEC_DISCOVER);
        }
        break;

    case ADAPTER_STATE_CEC_DISCOVER:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (!audio_system_found) {
            adapter_set_state(state, ADAPTER_STATE_CEC_SCAN);
        } else if (arc_system_audio_enabled() || arc_initiated) {
            adapter_set_state(state, ADAPTER_STATE_ARC_INIT);
        }
        break;

    case ADAPTER_STATE_ARC_INIT:
        if (!hdmi_connected) {
            adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
        } else if (!audio_system_found) {
            adapter_set_state(state, ADAPTER_STATE_CEC_SCAN);
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
        if (!*usb_requested) {
            realtime_set_usb_attached(true);
            *usb_requested = true;
        }
        if (realtime_usb_attached()) {
            printf("adapter: USB audio attached to host\n");
            adapter_set_state(state, ADAPTER_STATE_USB_IDLE);
        }
        break;

    case ADAPTER_STATE_USB_IDLE:
        if (realtime_usb_streaming()) {
            adapter_set_state(state, ADAPTER_STATE_USB_STREAMING);
        }
        break;

    case ADAPTER_STATE_USB_STREAMING:
        if (!realtime_usb_streaming()) {
            adapter_set_state(state, ADAPTER_STATE_USB_IDLE);
        }
        break;

    case ADAPTER_STATE_USB_DETACH:
        if (*usb_requested) {
            realtime_set_usb_attached(false);
            *usb_requested = false;
        }
        if (realtime_usb_attached()) {
            break;
        }
        {
            printf("adapter: USB audio detached from host\n");
            if (!hdmi_connected) {
                adapter_set_state(state, ADAPTER_STATE_WAIT_HDMI);
            } else if (!audio_system_found) {
                adapter_set_state(state, ADAPTER_STATE_CEC_SCAN);
            } else if (!arc_initiated) {
                adapter_set_state(state, ADAPTER_STATE_ARC_INIT);
            } else {
                adapter_set_state(state, ADAPTER_STATE_QUERY_CAPS);
            }
        }
        break;

    case ADAPTER_STATE_USB_RECOVERY:
        if (!time_reached(usb_recovery_reconnect_at)) {
            break;
        }
#if PICOARC_DEBUG_USB
        if (!*usb_requested) {
            if (realtime_usb_attached()) {
                break;
            }
            realtime_set_usb_attached(true);
            *usb_requested = true;
        } else if (realtime_usb_attached()) {
            printf("adapter: USB audio reattached to host\n");
            adapter_set_state(state, ADAPTER_STATE_DEBUG_USB);
        }
#else
        if (!realtime_usb_attached()) {
            adapter_set_state(state, ADAPTER_STATE_QUERY_CAPS);
        }
#endif
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
    const uint32_t phase_ms = elapsed_ms % PICOARC_STATUS_LED_WAIT_USB_BREATHE_MS;
    const uint32_t half_ms = PICOARC_STATUS_LED_WAIT_USB_BREATHE_MS / 2;
    const uint32_t ramp_ms = phase_ms < half_ms ? phase_ms : PICOARC_STATUS_LED_WAIT_USB_BREATHE_MS - phase_ms;
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
            *next_transition = make_timeout_time_ms(PICOARC_STATUS_LED_WAIT_HDMI_BLINK_MS);
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
        *next_transition = make_timeout_time_ms(PICOARC_STATUS_LED_WAIT_HDMI_BLINK_MS);
    }
}

int main(void) {
    const bool watchdog_reboot = watchdog_caused_reboot();
    watchdog_enable(PICOARC_WATCHDOG_TIMEOUT_MS, true);
    picoarc_log_init();

    // GP5 is an active-high FET gate: high holds HDMI HPD low, low releases it.
    // Keep HPD deasserted until the EDID responder is installed below.
#if PICOARC_HPD_GATE_ENABLE
    hpd_gate_hold(PICOARC_HPD_GATE_PIN);
#endif

    // Core 1 owns TinyUSB, UAC2, and S/PDIF for the rest of the boot. It does
    // not pump USB until the chained S/PDIF DMA path is ready.
    realtime_start(PICOARC_SPDIF_PIN);
#if PICOARC_DDC_EDID_ENABLE
    ddc_edid_init(PICOARC_DDC_EDID_SDA_PIN, PICOARC_DDC_EDID_SCL_PIN);
#endif
#if PICOARC_HPD_GATE_ENABLE
    hpd_gate_release(PICOARC_HPD_GATE_PIN);
#endif
    arc_init(PICOARC_CEC_PIN, PICOARC_HDMI_5V_PIN);
    picoarc_audio_caps_t audio_caps;
    arc_get_audio_caps_snapshot(&audio_caps);
    realtime_set_audio_caps(&audio_caps);
    cec_set_yield(cec_yield_pump);

    const uint led_pin = PICO_DEFAULT_LED_PIN;
    gpio_init(led_pin);
    gpio_set_dir(led_pin, GPIO_OUT);
    gpio_put(led_pin, false);

    printf("\nPicoARC bring-up\n");

    if (watchdog_reboot) {
        printf("adapter: recovered from watchdog reset\n");
    }

    printf("spdif: GP%d 48k stereo, core 1 owned\n", PICOARC_SPDIF_PIN);
    printf("adapter: idle audio policy=%s\n", PICOARC_IDLE_AUDIO_POLICY);
#if PICOARC_DEBUG_USB
    printf("adapter: debug USB stays online; audio streaming remains ARC/SAD gated\n");
#else
    printf("adapter: release USB waits for HDMI ARC capabilities and OSD name before enumeration\n");
#endif

    absolute_time_t next_led_transition = make_timeout_time_ms(PICOARC_STATUS_LED_WAIT_HDMI_BLINK_MS);
    uint32_t led_breath_started_ms = 0;
    bool led_on = false;
    status_led_mode_t led_mode = STATUS_LED_OFF;
    bool usb_requested = PICOARC_DEBUG_USB;
    adapter_state_t adapter_state =
#if PICOARC_DEBUG_USB
        ADAPTER_STATE_DEBUG_USB;
#else
        ADAPTER_STATE_BOOT;
#endif

    while (true) {
#if PICOARC_DDC_EDID_ENABLE
        ddc_edid_task();
#endif
        drain_realtime_requests();
        arc_task();
        arc_get_audio_caps_snapshot(&audio_caps);
        realtime_set_audio_caps(&audio_caps);
        adapter_task(&adapter_state, &usb_requested);

        const bool hdmi_connected = arc_hdmi_connected();
        const bool usb_enumerated = realtime_usb_attached() && realtime_usb_mounted();
        const status_led_mode_t next_led_mode =
            !hdmi_connected ? STATUS_LED_WAIT_HDMI : (!usb_enumerated ? STATUS_LED_WAIT_USB : STATUS_LED_OFF);
        status_led_task(led_pin, next_led_mode, &led_mode, &led_on, &next_led_transition, &led_breath_started_ms);

        watchdog_update();
        tight_loop_contents();
    }
}
