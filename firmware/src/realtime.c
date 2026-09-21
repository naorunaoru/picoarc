#include "realtime.h"

#include <string.h>

#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "picoarc_config.h"
#include "picoarc_log.h"
#include "cec_status_mailbox.h"
#include "pico/usb_reset_interface.h"
#include "spdif.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

typedef struct {
    bool ready;
    bool desired_usb_attached;
    bool usb_attached;
    bool usb_mounted;
    bool usb_streaming;
    unsigned int spdif_buffered_frames;
    bool usb_recovery_pending;

    picoarc_audio_caps_t audio_caps;
    uint32_t audio_caps_generation;
    char audio_name[33];
    uint32_t audio_name_generation;

    cec_status_mailbox_t cec_status;

    bool volume_request_pending;
    uint8_t requested_volume;
    bool mute_request_pending;
    bool requested_mute;
    bool reset_pending;
    realtime_reset_request_t reset_request;
} realtime_shared_t;

static critical_section_t shared_lock;
static realtime_shared_t shared;
static unsigned int realtime_spdif_pin;

// multicore_launch_core1() uses this linker-reserved range in SCRATCH_X.
// Fill the existing stack rather than supplying a new one so instrumentation
// does not change its size, placement, or SRAM-bank contention.
extern uint32_t __StackOneBottom;
extern uint32_t __StackOneTop;

enum {
    CORE1_STACK_CANARY_WORDS = 8,
};

#define CORE1_STACK_FILL UINT32_C(0xa5a5a5a5)

static unsigned int core1_stack_high_water_bytes;
static bool core1_stack_canary_ok = true;

static void core1_stack_fill(void) {
    uint32_t *const bottom = &__StackOneBottom;
    uint32_t *const top = &__StackOneTop;

    for (uint32_t *cursor = bottom; cursor < top; cursor++) {
        *cursor = CORE1_STACK_FILL;
    }
}

void realtime_core1_stack_stats(realtime_core1_stack_stats_t *stats) {
    if (!stats) {
        return;
    }

    const uint32_t *const bottom = &__StackOneBottom;
    const uint32_t *const top = &__StackOneTop;
    const unsigned int stack_words = (unsigned int)(top - bottom);
    unsigned int untouched_words = 0;

    while (untouched_words < stack_words &&
           bottom[untouched_words] == CORE1_STACK_FILL) {
        untouched_words++;
    }

    const unsigned int used_bytes =
        (stack_words - untouched_words) * sizeof(uint32_t);
    if (used_bytes > core1_stack_high_water_bytes) {
        core1_stack_high_water_bytes = used_bytes;
    }

    const unsigned int canary_words =
        stack_words < CORE1_STACK_CANARY_WORDS ?
            stack_words : CORE1_STACK_CANARY_WORDS;
    for (unsigned int i = 0; i < canary_words; i++) {
        if (bottom[i] != CORE1_STACK_FILL) {
            core1_stack_canary_ok = false;
            break;
        }
    }

    stats->size_bytes = stack_words * sizeof(uint32_t);
    stats->high_water_bytes = core1_stack_high_water_bytes;
    stats->free_bytes = stats->size_bytes - core1_stack_high_water_bytes;
    stats->canary_ok = core1_stack_canary_ok;
}

static bool audio_caps_equal(const picoarc_audio_caps_t *left,
                             const picoarc_audio_caps_t *right) {
    return left->arc_initiated == right->arc_initiated &&
           left->caps_ready == right->caps_ready &&
           left->pcm_rates_16 == right->pcm_rates_16 &&
           left->pcm_rates_20 == right->pcm_rates_20 &&
           left->pcm_rates_24 == right->pcm_rates_24 &&
           left->ac3_rates == right->ac3_rates &&
           left->dts_rates == right->dts_rates;
}

static spdif_mode_t idle_spdif_mode(void) {
#if PICOARC_IDLE_AUDIO_KEEPALIVE
    return SPDIF_MODE_SILENCE;
#else
    return SPDIF_MODE_OFF;
#endif
}

static void publish_usb_state(bool attached) {
    const bool mounted = attached && tud_mounted();
    const bool streaming = usb_audio_is_streaming();

    critical_section_enter_blocking(&shared_lock);
    shared.usb_attached = attached;
    shared.usb_mounted = mounted;
    shared.usb_streaming = streaming;
    shared.spdif_buffered_frames = spdif_buffered_frames();
    critical_section_exit(&shared_lock);
}

static void apply_core0_controls(bool *attached,
                                 uint32_t *audio_caps_generation,
                                 uint32_t *audio_name_generation) {
    bool desired_attached;
    bool apply_caps = false;
    picoarc_audio_caps_t audio_caps;
    bool apply_audio_name = false;
    char audio_name[sizeof(shared.audio_name)];
    bool apply_cec_status;
    cec_status_mailbox_t cec_status;

    critical_section_enter_blocking(&shared_lock);
    desired_attached = shared.desired_usb_attached;
    if (*audio_caps_generation != shared.audio_caps_generation) {
        *audio_caps_generation = shared.audio_caps_generation;
        audio_caps = shared.audio_caps;
        apply_caps = true;
    }
    if (*audio_name_generation != shared.audio_name_generation) {
        *audio_name_generation = shared.audio_name_generation;
        memcpy(audio_name, shared.audio_name, sizeof(audio_name));
        apply_audio_name = true;
    }

    apply_cec_status = cec_status_take(&shared.cec_status, &cec_status);
    critical_section_exit(&shared_lock);

    if (apply_caps) {
        usb_audio_set_arc_caps(&audio_caps);
    }
    if (apply_audio_name) {
        if (audio_name[0]) {
            usb_descriptors_set_audio_name(audio_name);
        } else {
            usb_descriptors_reset_audio_name();
        }
    }
    if (apply_cec_status && cec_status.volume_valid) {
        usb_audio_set_cec_audio_status(cec_status.volume,
                                       cec_status.muted,
                                       cec_status.notify_host);
    } else if (apply_cec_status) {
        usb_audio_set_cec_mute_status(cec_status.muted,
                                      cec_status.notify_host);
    }

    if (desired_attached == *attached) {
        return;
    }

    if (desired_attached) {
        tud_connect();
    } else {
        usb_audio_stop_streaming();
        tud_disconnect();
    }
    *attached = desired_attached;
}

static void realtime_core_main(void) {
    bool attached = PICOARC_DEBUG_USB;
    uint32_t audio_caps_generation = UINT32_MAX;
    uint32_t audio_name_generation = UINT32_MAX;

    // All USB initialization happens on core 1 so USBCTRL_IRQ is enabled only
    // in that core's NVIC. S/PDIF is then started before USB work is pumped.
    usb_descriptors_init();
    tusb_init();
#if !PICOARC_DEBUG_USB
    tud_disconnect();
#endif

    spdif_set_mode(idle_spdif_mode());
    spdif_start(realtime_spdif_pin);

    critical_section_enter_blocking(&shared_lock);
    shared.usb_attached = attached;
    shared.ready = true;
    critical_section_exit(&shared_lock);

    while (true) {
        apply_core0_controls(&attached,
                             &audio_caps_generation,
                             &audio_name_generation);

        tud_task();
        usb_audio_task();
        spdif_task();
        picoarc_log_task();

        if (usb_audio_take_recovery_request()) {
            usb_audio_stop_streaming();
            critical_section_enter_blocking(&shared_lock);
            shared.usb_recovery_pending = true;
            critical_section_exit(&shared_lock);
        }

        publish_usb_state(attached);
        tight_loop_contents();
    }
}

void realtime_start(unsigned int spdif_pin) {
    critical_section_init(&shared_lock);
    memset(&shared, 0, sizeof(shared));
    shared.desired_usb_attached = PICOARC_DEBUG_USB;
    realtime_spdif_pin = spdif_pin;
    core1_stack_high_water_bytes = 0;
    core1_stack_canary_ok = true;
    core1_stack_fill();
    multicore_launch_core1(realtime_core_main);

    while (true) {
        critical_section_enter_blocking(&shared_lock);
        const bool ready = shared.ready;
        critical_section_exit(&shared_lock);
        if (ready) {
            return;
        }
        tight_loop_contents();
    }
}

void realtime_set_usb_attached(bool attached) {
    critical_section_enter_blocking(&shared_lock);
    shared.desired_usb_attached = attached;
    critical_section_exit(&shared_lock);
}

void realtime_set_audio_caps(const picoarc_audio_caps_t *caps) {
    if (!caps) {
        return;
    }

    critical_section_enter_blocking(&shared_lock);
    if (!audio_caps_equal(&shared.audio_caps, caps)) {
        shared.audio_caps = *caps;
        shared.audio_caps_generation++;
    }
    critical_section_exit(&shared_lock);
}

void realtime_reset_audio_name(void) {
    realtime_set_audio_name(NULL);
}

void realtime_set_audio_name(const char *name) {
    char next_name[sizeof(shared.audio_name)] = {0};
    if (name) {
        strncpy(next_name, name, sizeof(next_name) - 1);
    }

    critical_section_enter_blocking(&shared_lock);
    if (memcmp(shared.audio_name, next_name, sizeof(next_name)) != 0) {
        memcpy(shared.audio_name, next_name, sizeof(next_name));
        shared.audio_name_generation++;
    }
    critical_section_exit(&shared_lock);
}

void realtime_set_cec_audio_status(uint8_t volume, bool muted, bool notify_host) {
    critical_section_enter_blocking(&shared_lock);
    cec_status_post_full(&shared.cec_status, volume, muted, notify_host);
    critical_section_exit(&shared_lock);
}

void realtime_set_cec_mute_status(bool muted, bool notify_host) {
    critical_section_enter_blocking(&shared_lock);
    cec_status_post_mute(&shared.cec_status, muted, notify_host);
    critical_section_exit(&shared_lock);
}

bool realtime_usb_attached(void) {
    critical_section_enter_blocking(&shared_lock);
    const bool attached = shared.usb_attached;
    critical_section_exit(&shared_lock);
    return attached;
}

bool realtime_usb_mounted(void) {
    critical_section_enter_blocking(&shared_lock);
    const bool mounted = shared.usb_mounted;
    critical_section_exit(&shared_lock);
    return mounted;
}

bool realtime_usb_streaming(void) {
    critical_section_enter_blocking(&shared_lock);
    const bool streaming = shared.usb_streaming;
    critical_section_exit(&shared_lock);
    return streaming;
}

unsigned int realtime_spdif_buffered_frames(void) {
    critical_section_enter_blocking(&shared_lock);
    const unsigned int buffered = shared.spdif_buffered_frames;
    critical_section_exit(&shared_lock);
    return buffered;
}

bool realtime_take_usb_recovery_request(void) {
    critical_section_enter_blocking(&shared_lock);
    const bool pending = shared.usb_recovery_pending;
    shared.usb_recovery_pending = false;
    critical_section_exit(&shared_lock);
    return pending;
}

#if PICOARC_DEBUG_USB
// TinyUSB invokes this callback from core 1. The BOOTSEL ROM path arms a short
// watchdog reset, while core 0 normally feeds that watchdog. Defer the reset
// to core 0 so it stops feeding before entering the ROM path.
void tud_cdc_line_coding_cb(uint8_t itf,
                            cdc_line_coding_t const *line_coding) {
    (void)itf;
    if (line_coding->bit_rate != PICO_STDIO_USB_RESET_MAGIC_BAUD_RATE) {
        return;
    }

    critical_section_enter_blocking(&shared_lock);
    shared.reset_request = (realtime_reset_request_t){
        .activity_gpio = -1,
        .bootsel_disable_mask = PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK,
    };
    shared.reset_pending = true;
    critical_section_exit(&shared_lock);
}
#endif

bool realtime_take_reset_request(realtime_reset_request_t *request) {
    critical_section_enter_blocking(&shared_lock);
    const bool pending = shared.reset_pending;
    if (pending && request) {
        *request = shared.reset_request;
    }
    shared.reset_pending = false;
    critical_section_exit(&shared_lock);
    return pending;
}

#if PICOARC_DEBUG_USB
static uint8_t reset_interface_number;

static void resetd_init(void) {
    reset_interface_number = UINT8_MAX;
}

static void resetd_reset(uint8_t rhport) {
    (void)rhport;
    reset_interface_number = UINT8_MAX;
}

static uint16_t resetd_open(uint8_t rhport, tusb_desc_interface_t const *desc,
                            uint16_t max_len) {
    (void)rhport;
    TU_VERIFY(max_len >= sizeof(*desc), 0);
    TU_VERIFY(desc->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC &&
              desc->bInterfaceSubClass == RESET_INTERFACE_SUBCLASS &&
              desc->bInterfaceProtocol == RESET_INTERFACE_PROTOCOL, 0);
    reset_interface_number = desc->bInterfaceNumber;
    return sizeof(*desc);
}

static bool resetd_control(uint8_t rhport, uint8_t stage,
                           tusb_control_request_t const *request) {
    (void)rhport;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->wIndex != reset_interface_number) return false;

    realtime_reset_request_t reset = { .activity_gpio = -1 };
    if (request->bRequest == RESET_REQUEST_BOOTSEL) {
        reset.bootsel_disable_mask =
            (request->wValue & 0x7fu) |
            PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK;
        if (request->wValue & 0x100u) reset.activity_gpio = request->wValue >> 9u;
        reset.activity_gpio_active_low = (request->wValue & 0x200u) != 0;
    } else if (request->bRequest == RESET_REQUEST_FLASH) {
        reset.flash_boot = true;
    } else {
        return false;
    }
    critical_section_enter_blocking(&shared_lock);
    shared.reset_request = reset;
    shared.reset_pending = true;
    critical_section_exit(&shared_lock);
    return true;
}

static bool resetd_xfer(uint8_t rhport, uint8_t ep_addr,
                        xfer_result_t result, uint32_t transferred) {
    (void)rhport; (void)ep_addr; (void)result; (void)transferred;
    return true;
}

static usbd_class_driver_t const reset_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "RESET",
#endif
    .init = resetd_init,
    .reset = resetd_reset,
    .open = resetd_open,
    .control_xfer_cb = resetd_control,
    .xfer_cb = resetd_xfer,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count) {
    *count = 1;
    return &reset_driver;
}
#endif

void realtime_post_volume_request(uint8_t volume) {
    critical_section_enter_blocking(&shared_lock);
    shared.requested_volume = volume;
    shared.volume_request_pending = true;
    critical_section_exit(&shared_lock);
}

void realtime_post_mute_request(bool muted) {
    critical_section_enter_blocking(&shared_lock);
    shared.requested_mute = muted;
    shared.mute_request_pending = true;
    critical_section_exit(&shared_lock);
}

bool realtime_take_volume_request(uint8_t *volume) {
    critical_section_enter_blocking(&shared_lock);
    const bool pending = shared.volume_request_pending;
    if (pending && volume) {
        *volume = shared.requested_volume;
    }
    shared.volume_request_pending = false;
    critical_section_exit(&shared_lock);
    return pending;
}

bool realtime_take_mute_request(bool *muted) {
    critical_section_enter_blocking(&shared_lock);
    const bool pending = shared.mute_request_pending;
    if (pending && muted) {
        *muted = shared.requested_mute;
    }
    shared.mute_request_pending = false;
    critical_section_exit(&shared_lock);
    return pending;
}
