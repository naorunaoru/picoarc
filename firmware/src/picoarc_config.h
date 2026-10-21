#ifndef PICOARC_CONFIG_H
#define PICOARC_CONFIG_H

// Board-level GPIO assignments.
#define PICOARC_SPDIF_PIN 2
#define PICOARC_CEC_PIN 3
#define PICOARC_HDMI_5V_PIN 4
#define PICOARC_HDMI_5V_DEBOUNCE_MS 100

// Active-high FET gate: high holds HDMI HPD low, low releases HPD.
#define PICOARC_HPD_GATE_ENABLE 1
#define PICOARC_HPD_GATE_PIN 5
#define PICOARC_HPD_REPLUG_LOW_MS 200

// Seconds to keep sending silence after the host closes its audio stream.
// 0 stops the carrier immediately; -1 keeps sending silence forever.
#ifndef PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS
#define PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS 300
#endif
#if PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS < -1
#error "PICOARC_IDLE_AUDIO_TIMEOUT_SECONDS must be -1, 0, or positive seconds"
#endif

// Status LED user-visible timing.
#define PICOARC_STATUS_LED_WAIT_HDMI_BLINK_MS 1000
#define PICOARC_STATUS_LED_WAIT_USB_BREATHE_MS 2400

// Recover when a host selects a streaming alternate setting but does not keep
// delivering isochronous audio. Keep the hardware watchdog long enough for
// synchronous CEC transactions while still recovering a wedged main loop.
#define PICOARC_USB_STREAM_PACKET_TIMEOUT_MS 1500
#define PICOARC_USB_RECOVERY_DISCONNECT_MS 500
#define PICOARC_WATCHDOG_TIMEOUT_MS 8000

// Answer HDMI DDC EDID reads from the RP2040 through the board-level I2C
// translator. HDMI DDC is a 5V bus; do not connect pins 15/16 directly to
// RP2040 GPIO outside a bench-only bodge.
#define PICOARC_DDC_EDID_ENABLE 1
#define PICOARC_DDC_EDID_SDA_PIN 6
#define PICOARC_DDC_EDID_SCL_PIN 7
#define PICOARC_DDC_EDID_CEC_READY_BYTES 256
#define PICOARC_DDC_EDID_CEC_SETTLE_MS 750
#define PICOARC_DDC_EDID_CEC_NO_READ_TIMEOUT_MS 3000

// Optional CEC audio probing
#define PICOARC_CEC_QUERY_EXTRA_SADS 0

#endif
