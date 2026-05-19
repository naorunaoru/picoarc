#include "cec.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "cec_tx.pio.h"
#include "pico/stdlib.h"

#include <stdio.h>

enum {
    CEC_START_LOW_US = 3700,
    CEC_START_HIGH_US = 800,
    CEC_BIT_TOTAL_US = 2400,
    CEC_ZERO_LOW_US = 1500,
    CEC_ONE_LOW_US = 600,
    CEC_BIT_SAMPLE_US = 1050,
    CEC_ACK_SAMPLE_US = 1250,
    CEC_ACK_LATE_SAMPLE_US = 1450,
    CEC_ACK_RELEASE_MAX_US = 2050,
    CEC_ACK_HIGH_NOT_SEEN_US = 0xffff,

    CEC_IDLE_BEFORE_TX_US = 5000,
    CEC_EDGE_TIMEOUT_US = 6000,
    CEC_PASSIVE_GAP_US = 10000,
    CEC_EDGE_RING_SIZE = 512,
    CEC_TX_WORDS_PER_SEGMENT = 2 * (1 + 9),
};

static const bool cec_trace_tx = false;

typedef struct {
    uint32_t time_us;
    bool level;
} cec_edge_t;

typedef struct {
    bool low_at_1050_us;
    bool low_at_1250_us;
    bool low_at_1450_us;
    bool low_at_end;
    bool released_as_input;
    uint16_t high_after_release_us;
    uint16_t high_after_end_us;
} cec_ack_info_t;

typedef enum {
    PASSIVE_WAIT_START_FALL,
    PASSIVE_WAIT_START_RISE,
    PASSIVE_WAIT_BIT_FALL,
    PASSIVE_WAIT_BIT_RISE,
} cec_passive_state_t;

static unsigned int cec_pin;
static PIO cec_tx_pio;
static unsigned int cec_tx_sm;
static unsigned int cec_tx_dma_chan;
static dma_channel_config cec_tx_dma_config;
static uint64_t cec_tx_busy_until_us;
static uint16_t own_logical_address_mask = 1u << CEC_LOGICAL_TV;
static volatile cec_edge_t cec_edges[CEC_EDGE_RING_SIZE];
static volatile unsigned int cec_edge_read;
static volatile unsigned int cec_edge_write;

static cec_passive_state_t passive_state = PASSIVE_WAIT_START_FALL;
static cec_frame_t passive_frame;
static uint32_t passive_fall_us;
static uint32_t passive_last_edge_us;
static uint8_t passive_byte;
static unsigned int passive_bit_index;
static bool passive_expect_ack;
static bool passive_should_ack;
static bool passive_complete_after_ack;
static uint32_t cec_tx_words[CEC_TX_WORDS_PER_SEGMENT];

static uint64_t now_us(void) {
    return time_us_64();
}

static void release_bus(void) {
    gpio_set_dir(cec_pin, GPIO_IN);
}

static void drive_low(void) {
    gpio_put(cec_pin, 0);
    gpio_set_dir(cec_pin, GPIO_OUT);
}

static uint32_t tx_duration_word(uint32_t duration_us) {
    return duration_us > 2 ? duration_us - 2 : 0;
}

static void tx_append_symbol(uint32_t *words, size_t *count, uint32_t *duration_us,
                             uint32_t low_us, uint32_t high_us) {
    words[(*count)++] = tx_duration_word(low_us);
    words[(*count)++] = tx_duration_word(high_us);
    *duration_us += low_us + high_us;
}

static void tx_append_bit(uint32_t *words, size_t *count, uint32_t *duration_us, bool value) {
    const uint32_t low_us = value ? CEC_ONE_LOW_US : CEC_ZERO_LOW_US;
    tx_append_symbol(words, count, duration_us, low_us, CEC_BIT_TOTAL_US - low_us);
}

static void tx_pin_to_sio(void) {
    pio_sm_set_enabled(cec_tx_pio, cec_tx_sm, false);
    gpio_set_function(cec_pin, GPIO_FUNC_SIO);
    gpio_put(cec_pin, 0);
    release_bus();
}

static void tx_pin_to_pio(void) {
    gpio_set_function(cec_pin, GPIO_FUNC_PIO0);
    pio_sm_set_pins(cec_tx_pio, cec_tx_sm, 0);
    pio_sm_set_consecutive_pindirs(cec_tx_pio, cec_tx_sm, cec_pin, 1, false);
}

static void tx_service(void) {
    if (cec_tx_busy_until_us != 0 && now_us() >= cec_tx_busy_until_us) {
        cec_tx_busy_until_us = 0;
        tx_pin_to_sio();
    }
}

static void tx_wait_idle(void) {
    while (cec_tx_busy_until_us != 0) {
        tx_service();
        tight_loop_contents();
    }
}

static void tx_start_symbols(const uint32_t *words, size_t count, uint32_t duration_us) {
    if (count == 0) {
        return;
    }

    tx_wait_idle();
    tx_pin_to_pio();
    pio_sm_set_enabled(cec_tx_pio, cec_tx_sm, false);
    pio_sm_clear_fifos(cec_tx_pio, cec_tx_sm);
    pio_sm_restart(cec_tx_pio, cec_tx_sm);
    pio_sm_set_enabled(cec_tx_pio, cec_tx_sm, true);

    const uint64_t start_us = now_us();
    dma_channel_configure(cec_tx_dma_chan,
                          &cec_tx_dma_config,
                          &cec_tx_pio->txf[cec_tx_sm],
                          words,
                          count,
                          true);

    cec_tx_busy_until_us = start_us + duration_us + 20;
}

static void tx_run_symbols(const uint32_t *words, size_t count, uint32_t duration_us) {
    tx_start_symbols(words, count, duration_us);
    tx_wait_idle();
}

static void cec_gpio_irq(uint gpio, uint32_t events) {
    if (gpio != cec_pin || !(events & (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE))) {
        return;
    }

    const unsigned int write = cec_edge_write;
    const unsigned int next_write = (write + 1) % CEC_EDGE_RING_SIZE;
    if (next_write == cec_edge_read) {
        cec_edge_read = (cec_edge_read + 1) % CEC_EDGE_RING_SIZE;
    }

    cec_edges[write] = (cec_edge_t){
        .time_us = time_us_32(),
        .level = gpio_get(cec_pin),
    };
    cec_edge_write = next_write;
}

static bool pop_edge(cec_edge_t *edge) {
    const unsigned int read = cec_edge_read;
    if (read == cec_edge_write) {
        return false;
    }

    *edge = cec_edges[read];
    cec_edge_read = (read + 1) % CEC_EDGE_RING_SIZE;
    return true;
}

static bool wait_idle(uint32_t idle_us, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;
    uint64_t high_since = gpio_get(cec_pin) ? now_us() : 0;

    while (now_us() < deadline) {
        tx_service();

        const uint64_t now = now_us();
        if (gpio_get(cec_pin)) {
            if (high_since == 0) {
                high_since = now;
            } else if (now - high_since >= idle_us) {
                return true;
            }
        } else {
            high_since = 0;
        }
        tight_loop_contents();
    }

    return false;
}

static void wait_until_us(uint64_t target_us) {
    while (now_us() < target_us) {
        tight_loop_contents();
    }
}

static uint16_t elapsed_since_start_us(uint64_t start_us, uint64_t now) {
    const uint64_t elapsed = now - start_us;
    return elapsed < CEC_ACK_HIGH_NOT_SEEN_US ? (uint16_t)elapsed : CEC_ACK_HIGH_NOT_SEEN_US;
}

static uint16_t wait_for_high_until_us(uint64_t start_us, uint64_t deadline_us) {
    while (now_us() < deadline_us) {
        if (gpio_get(cec_pin)) {
            return elapsed_since_start_us(start_us, now_us());
        }
        tight_loop_contents();
    }

    return CEC_ACK_HIGH_NOT_SEEN_US;
}

static void trace_frame_prefix(const char *label, const uint8_t *bytes, size_t len) {
    if (!cec_trace_tx || len == 0) {
        return;
    }

    const uint8_t header = bytes[0];
    printf("cec: %s +%llums %x->%x",
           label,
           (unsigned long long)(time_us_64() / 1000),
           header >> 4,
           header & 0x0f);
    for (size_t i = 1; i < len; i++) {
        printf(" %02x", bytes[i]);
    }
}

static void trace_ack_info(const cec_ack_info_t *ack_info) {
    if (!ack_info) {
        return;
    }

    printf(" ackbits=%u%u%u%u in=%u hi=%u post=%u",
           ack_info->low_at_1050_us ? 1u : 0u,
           ack_info->low_at_1250_us ? 1u : 0u,
           ack_info->low_at_1450_us ? 1u : 0u,
           ack_info->low_at_end ? 1u : 0u,
           ack_info->released_as_input ? 1u : 0u,
           ack_info->high_after_release_us,
           ack_info->high_after_end_us);
}

static void trace_send_result(const uint8_t *bytes, size_t len, bool local_directed,
                              bool broadcast, bool acked, const cec_ack_info_t *ack_info) {
    if (!cec_trace_tx || len == 0) {
        return;
    }

    trace_frame_prefix("tx", bytes, len);
    printf(" result=%s mode=%s",
           acked ? "ok" : "fail",
           broadcast ? "broadcast" : (local_directed ? "local" : "direct"));
    trace_ack_info(ack_info);
    printf("\n");
}

static bool sample_ack_slot(cec_ack_info_t *ack_info) {
    tx_pin_to_sio();
    const uint64_t start_us = now_us();
    drive_low();
    wait_until_us(start_us + CEC_ONE_LOW_US);
    release_bus();
    const bool released_as_input = !gpio_get_dir(cec_pin);
    uint16_t high_after_release_us = CEC_ACK_HIGH_NOT_SEEN_US;

    high_after_release_us = wait_for_high_until_us(start_us, start_us + CEC_BIT_SAMPLE_US);
    const bool low_at_1050_us = !gpio_get(cec_pin);
    if (high_after_release_us == CEC_ACK_HIGH_NOT_SEEN_US) {
        high_after_release_us = wait_for_high_until_us(start_us, start_us + CEC_ACK_SAMPLE_US);
    } else {
        wait_until_us(start_us + CEC_ACK_SAMPLE_US);
    }
    const bool low_at_1250_us = !gpio_get(cec_pin);
    if (high_after_release_us == CEC_ACK_HIGH_NOT_SEEN_US) {
        high_after_release_us = wait_for_high_until_us(start_us, start_us + CEC_ACK_LATE_SAMPLE_US);
    } else {
        wait_until_us(start_us + CEC_ACK_LATE_SAMPLE_US);
    }
    const bool low_at_1450_us = !gpio_get(cec_pin);
    if (high_after_release_us == CEC_ACK_HIGH_NOT_SEEN_US) {
        high_after_release_us = wait_for_high_until_us(start_us, start_us + CEC_BIT_TOTAL_US);
    } else {
        wait_until_us(start_us + CEC_BIT_TOTAL_US);
    }
    const bool low_at_end = !gpio_get(cec_pin);
    const uint16_t high_after_end_us =
        ack_info && low_at_end ? wait_for_high_until_us(start_us, start_us + CEC_EDGE_TIMEOUT_US)
                               : CEC_ACK_HIGH_NOT_SEEN_US;

    if (ack_info) {
        *ack_info = (cec_ack_info_t){
            .low_at_1050_us = low_at_1050_us,
            .low_at_1250_us = low_at_1250_us,
            .low_at_1450_us = low_at_1450_us,
            .low_at_end = low_at_end,
            .released_as_input = released_as_input,
            .high_after_release_us = high_after_release_us,
            .high_after_end_us = high_after_end_us,
        };
    }

    const bool acked = low_at_1250_us &&
                       high_after_release_us != CEC_ACK_HIGH_NOT_SEEN_US &&
                       high_after_release_us <= CEC_ACK_RELEASE_MAX_US;
    return acked;
}

static bool send_local_ack_slot(cec_ack_info_t *ack_info) {
    tx_pin_to_sio();
    const uint64_t start_us = now_us();
    drive_low();
    wait_until_us(start_us + CEC_ZERO_LOW_US);
    release_bus();
    const bool released_as_input = !gpio_get_dir(cec_pin);
    const uint16_t high_after_release_us =
        wait_for_high_until_us(start_us, start_us + CEC_BIT_TOTAL_US);
    wait_until_us(start_us + CEC_BIT_TOTAL_US);

    if (ack_info) {
        *ack_info = (cec_ack_info_t){
            .low_at_1050_us = true,
            .low_at_1250_us = true,
            .low_at_1450_us = true,
            .low_at_end = !gpio_get(cec_pin),
            .released_as_input = released_as_input,
            .high_after_release_us = high_after_release_us,
            .high_after_end_us = CEC_ACK_HIGH_NOT_SEEN_US,
        };
    }

    return released_as_input &&
           high_after_release_us != CEC_ACK_HIGH_NOT_SEEN_US &&
           high_after_release_us <= CEC_ACK_RELEASE_MAX_US;
}

void cec_init(unsigned int pin) {
    cec_pin = pin;
    gpio_init(cec_pin);
    gpio_put(cec_pin, 0);
    release_bus();
    gpio_set_irq_enabled_with_callback(cec_pin,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true,
                                       cec_gpio_irq);

    // TX shares PIO0 with the one-instruction SPDIF program and owns the CEC
    // pin output mux only while a frame or ACK bit is being sent.
    cec_tx_pio = pio0;
    cec_tx_sm = pio_claim_unused_sm(cec_tx_pio, true);
    const unsigned int offset = pio_add_program(cec_tx_pio, &cec_tx_program);

    pio_sm_config config = cec_tx_program_get_default_config(offset);
    sm_config_set_set_pins(&config, cec_pin, 1);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / cec_tx_CEC_TX_RATE_HZ);
    pio_sm_init(cec_tx_pio, cec_tx_sm, offset, &config);
    tx_pin_to_sio();

    cec_tx_dma_chan = dma_claim_unused_channel(true);
    cec_tx_dma_config = dma_channel_get_default_config(cec_tx_dma_chan);
    channel_config_set_transfer_data_size(&cec_tx_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&cec_tx_dma_config, true);
    channel_config_set_write_increment(&cec_tx_dma_config, false);
    channel_config_set_dreq(&cec_tx_dma_config, pio_get_dreq(cec_tx_pio, cec_tx_sm, true));
}

bool cec_bus_is_high(void) {
    return gpio_get(cec_pin);
}

void cec_set_logical_address(uint8_t logical_address) {
    own_logical_address_mask = 1u << (logical_address & 0x0f);
}

static bool cec_send_frame_internal(const uint8_t *bytes, size_t len, cec_ack_info_t *ack_info) {
    if (ack_info) {
        *ack_info = (cec_ack_info_t){0};
    }

    if (len == 0 || len > 16) {
        if (cec_trace_tx) {
            printf("cec: tx +%llums invalid len=%u\n",
                   (unsigned long long)(time_us_64() / 1000),
                   (unsigned int)len);
        }
        return false;
    }

    if (!wait_idle(CEC_IDLE_BEFORE_TX_US, 100000)) {
        trace_frame_prefix("tx-timeout", bytes, len);
        if (cec_trace_tx) {
            printf(" idle-timeout\n");
        }
        return false;
    }

    gpio_set_irq_enabled(cec_pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
    const bool broadcast = (bytes[0] & 0x0f) == CEC_LOGICAL_BROADCAST;
    const uint8_t source = bytes[0] >> 4;
    const uint8_t destination = bytes[0] & 0x0f;
    const bool local_directed =
        !broadcast &&
        (own_logical_address_mask & (1u << source)) != 0 &&
        (own_logical_address_mask & (1u << destination)) != 0;
    bool all_acked = true;
    cec_ack_info_t last_ack_info = {0};
    for (size_t i = 0; i < len; i++) {
        size_t count = 0;
        uint32_t duration_us = 0;
        if (i == 0) {
            tx_append_symbol(cec_tx_words, &count, &duration_us, CEC_START_LOW_US, CEC_START_HIGH_US);
        }
        for (int bit = 7; bit >= 0; bit--) {
            tx_append_bit(cec_tx_words, &count, &duration_us, (bytes[i] >> bit) & 1u);
        }

        const bool eom = i == len - 1;
        tx_append_bit(cec_tx_words, &count, &duration_us, eom);
        tx_run_symbols(cec_tx_words, count, duration_us);
        cec_ack_info_t byte_ack_info = {0};
        const bool acked = local_directed
                               ? send_local_ack_slot(&byte_ack_info)
                               : sample_ack_slot(&byte_ack_info);
        last_ack_info = byte_ack_info;
        if (i == len - 1 && ack_info) {
            *ack_info = byte_ack_info;
        }
        all_acked = all_acked && (broadcast ? !acked : acked);
    }

    release_bus();
    cec_passive_reset();
    gpio_set_irq_enabled(cec_pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    trace_send_result(bytes, len, local_directed, broadcast, all_acked, &last_ack_info);
    return all_acked;
}

bool cec_send_frame(const uint8_t *bytes, size_t len) {
    return cec_send_frame_internal(bytes, len, NULL);
}

bool cec_send_with_retry(const uint8_t *bytes, size_t len, unsigned int attempts) {
    for (unsigned int attempt = 0; attempt < attempts; attempt++) {
        if (cec_send_frame(bytes, len)) {
            return true;
        }
        sleep_ms(50);
    }

    return false;
}

bool cec_poll(uint8_t initiator, uint8_t follower) {
    const uint8_t header = (uint8_t)((initiator << 4) | (follower & 0x0f));
    return cec_send_frame_internal(&header, 1, NULL);
}

bool cec_receive_frame(cec_frame_t *frame, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;

    cec_passive_reset();
    while (now_us() < deadline) {
        if (cec_receive_frame_passive(frame)) {
            return true;
        }
        tight_loop_contents();
    }

    return false;
}

void cec_passive_reset(void) {
    cec_edge_read = cec_edge_write;
    passive_state = PASSIVE_WAIT_START_FALL;
    passive_frame = (cec_frame_t){0};
    passive_byte = 0;
    passive_bit_index = 0;
    passive_expect_ack = false;
    passive_should_ack = false;
    passive_complete_after_ack = false;
}

static void passive_start_frame(void) {
    passive_frame = (cec_frame_t){0};
    passive_byte = 0;
    passive_bit_index = 0;
    passive_expect_ack = false;
    passive_should_ack = false;
    passive_complete_after_ack = false;
}

static void passive_ack_slot(void) {
    size_t count = 0;
    uint32_t duration_us = 0;

    tx_append_symbol(cec_tx_words, &count, &duration_us,
                     CEC_ZERO_LOW_US, CEC_BIT_TOTAL_US - CEC_ZERO_LOW_US);
    tx_start_symbols(cec_tx_words, count, duration_us);
}

static bool passive_accept_bit(bool bit, cec_frame_t *frame) {
    if (passive_expect_ack) {
        passive_expect_ack = false;
        if (passive_complete_after_ack) {
            passive_frame.complete = true;
            *frame = passive_frame;
            passive_state = PASSIVE_WAIT_START_FALL;
            return true;
        }

        passive_state = PASSIVE_WAIT_BIT_FALL;
        return false;
    }

    if (passive_bit_index < 8) {
        passive_byte = (uint8_t)((passive_byte << 1) | (bit ? 1u : 0u));
        passive_bit_index++;
        return false;
    }

    if (passive_frame.len >= sizeof(passive_frame.bytes)) {
        passive_state = PASSIVE_WAIT_START_FALL;
        return false;
    }

    passive_frame.bytes[passive_frame.len++] = passive_byte;
    passive_byte = 0;
    passive_bit_index = 0;
    passive_expect_ack = true;

    const uint8_t destination = passive_frame.bytes[0] & 0x0f;
    passive_should_ack = destination != CEC_LOGICAL_BROADCAST &&
                         (own_logical_address_mask & (1u << destination)) != 0;
    passive_complete_after_ack = bit;
    return false;
}

bool cec_receive_frame_passive(cec_frame_t *frame) {
    cec_edge_t edge;

    tx_service();

    while (pop_edge(&edge)) {
        switch (passive_state) {
        case PASSIVE_WAIT_START_FALL:
            if (!edge.level) {
                passive_fall_us = edge.time_us;
                passive_last_edge_us = edge.time_us;
                passive_state = PASSIVE_WAIT_START_RISE;
            }
            break;

        case PASSIVE_WAIT_START_RISE:
            if (edge.level) {
                const uint32_t low_us = edge.time_us - passive_fall_us;
                passive_last_edge_us = edge.time_us;
                if (low_us >= 3200 && low_us <= 4300) {
                    passive_start_frame();
                    passive_state = PASSIVE_WAIT_BIT_FALL;
                } else {
                    passive_state = PASSIVE_WAIT_START_FALL;
                }
            } else {
                passive_fall_us = edge.time_us;
            }
            break;

        case PASSIVE_WAIT_BIT_FALL:
            if (!edge.level) {
                if ((uint32_t)(edge.time_us - passive_last_edge_us) > CEC_PASSIVE_GAP_US) {
                    passive_fall_us = edge.time_us;
                    passive_state = PASSIVE_WAIT_START_RISE;
                } else {
                    passive_fall_us = edge.time_us;
                    passive_state = PASSIVE_WAIT_BIT_RISE;
                    if (passive_expect_ack && passive_should_ack) {
                        passive_ack_slot();
                    }
                }
                passive_last_edge_us = edge.time_us;
            }
            break;

        case PASSIVE_WAIT_BIT_RISE:
            if (edge.level) {
                const uint32_t low_us = edge.time_us - passive_fall_us;
                passive_last_edge_us = edge.time_us;
                passive_state = PASSIVE_WAIT_BIT_FALL;
                if (low_us > CEC_EDGE_TIMEOUT_US) {
                    passive_state = PASSIVE_WAIT_START_FALL;
                    break;
                }
                if (passive_accept_bit(low_us < CEC_BIT_SAMPLE_US, frame)) {
                    return true;
                }
            } else {
                passive_fall_us = edge.time_us;
            }
            break;
        }
    }

    return false;
}
