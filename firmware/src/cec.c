#include "cec.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "cec_tx.pio.h"
#include "pico/stdlib.h"

// HDMI-CEC bit timings, nominal values in microseconds.
enum {
    CEC_START_LOW_US = 3700,
    CEC_START_HIGH_US = 800,
    CEC_BIT_TOTAL_US = 2400,
    CEC_ZERO_LOW_US = 1500,
    CEC_ONE_LOW_US = 600,
    CEC_ACK_SAMPLE_US = 1050,

    CEC_IDLE_BEFORE_TX_US = 5000,
    CEC_EDGE_TIMEOUT_US = 6000,
    CEC_PASSIVE_GAP_US = 10000,
    CEC_EDGE_RING_SIZE = 512,
    CEC_TX_PIO_RATE_HZ = 1000000,
    CEC_TX_WORDS_PER_SEGMENT = 2 * (1 + 9),
};

typedef struct {
    uint32_t time_us;
    bool level;
} cec_edge_t;

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
static uint8_t own_logical_address = CEC_LOGICAL_TV;
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

static void drive_low(void) {
    gpio_put(cec_pin, 0);
    gpio_set_dir(cec_pin, GPIO_OUT);
}

static void release_bus(void) {
    gpio_set_dir(cec_pin, GPIO_IN);
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
    gpio_set_function(cec_pin, GPIO_FUNC_PIO1);
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

static bool wait_level(bool high, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;
    while (gpio_get(cec_pin) != high) {
        if (now_us() >= deadline) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static bool wait_falling_edge(uint32_t timeout_us) {
    if (!wait_level(true, timeout_us)) {
        return false;
    }
    return wait_level(false, timeout_us);
}

static bool wait_rising_edge(uint32_t timeout_us, uint32_t *low_us) {
    const uint64_t start = now_us();
    if (!wait_level(true, timeout_us)) {
        return false;
    }
    *low_us = (uint32_t)(now_us() - start);
    return true;
}

static bool wait_idle(uint32_t idle_us, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;
    uint64_t high_since = 0;

    while (now_us() < deadline) {
        if (gpio_get(cec_pin)) {
            if (high_since == 0) {
                high_since = now_us();
            } else if (now_us() - high_since >= idle_us) {
                return true;
            }
        } else {
            high_since = 0;
        }
        sleep_us(50);
    }

    return false;
}

static bool send_ack_slot(void) {
    drive_low();
    sleep_us(CEC_ONE_LOW_US);
    release_bus();
    sleep_us(CEC_ACK_SAMPLE_US - CEC_ONE_LOW_US);
    const bool acked = !gpio_get(cec_pin);
    sleep_us(CEC_BIT_TOTAL_US - CEC_ACK_SAMPLE_US);
    return acked;
}

static bool read_bit(bool *value) {
    uint32_t low_us = 0;

    if (!wait_falling_edge(CEC_EDGE_TIMEOUT_US)) {
        return false;
    }
    if (!wait_rising_edge(CEC_EDGE_TIMEOUT_US, &low_us)) {
        return false;
    }

    *value = low_us < 1050;
    return true;
}

static void ack_after_initiator_edge(void) {
    if (!wait_level(false, CEC_EDGE_TIMEOUT_US)) {
        return;
    }

    sleep_us(CEC_ONE_LOW_US);
    drive_low();
    sleep_us(CEC_ZERO_LOW_US - CEC_ONE_LOW_US);
    release_bus();
    sleep_us(CEC_BIT_TOTAL_US - CEC_ZERO_LOW_US);
}

static void skip_ack_after_initiator_edge(void) {
    if (!wait_level(false, CEC_EDGE_TIMEOUT_US)) {
        return;
    }
    if (!wait_level(true, CEC_EDGE_TIMEOUT_US)) {
        return;
    }
    sleep_us(CEC_BIT_TOTAL_US - CEC_ONE_LOW_US);
}

void cec_init(unsigned int pin) {
    cec_pin = pin;
    gpio_init(cec_pin);
    gpio_put(cec_pin, 0);
    release_bus();
    gpio_set_irq_enabled_with_callback(cec_pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, cec_gpio_irq);

    cec_tx_pio = pio1;
    cec_tx_sm = pio_claim_unused_sm(cec_tx_pio, true);
    const unsigned int offset = pio_add_program(cec_tx_pio, &cec_tx_program);

    pio_sm_config config = cec_tx_program_get_default_config(offset);
    sm_config_set_set_pins(&config, cec_pin, 1);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / CEC_TX_PIO_RATE_HZ);
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
    own_logical_address = logical_address & 0x0f;
}

bool cec_send_frame(const uint8_t *bytes, size_t len) {
    if (len == 0 || len > 16) {
        return false;
    }

    if (!wait_idle(CEC_IDLE_BEFORE_TX_US, 100000)) {
        return false;
    }

    gpio_set_irq_enabled(cec_pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
    const bool broadcast = (bytes[0] & 0x0f) == CEC_LOGICAL_BROADCAST;
    bool all_acked = true;
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
        const bool acked = send_ack_slot();
        all_acked = all_acked && (broadcast ? !acked : acked);
    }

    release_bus();
    cec_passive_reset();
    gpio_set_irq_enabled(cec_pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    return all_acked;
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
    return cec_send_frame(&header, 1);
}

bool cec_receive_frame(cec_frame_t *frame, uint32_t timeout_us) {
    uint32_t start_low_us = 0;

    frame->len = 0;
    frame->complete = false;

    if (!wait_falling_edge(timeout_us)) {
        return false;
    }
    if (!wait_rising_edge(CEC_EDGE_TIMEOUT_US, &start_low_us)) {
        return false;
    }

    if (start_low_us < 3200 || start_low_us > 4300) {
        return false;
    }

    while (frame->len < sizeof(frame->bytes)) {
        uint8_t byte = 0;
        bool bit = false;

        for (int i = 0; i < 8; i++) {
            if (!read_bit(&bit)) {
                return false;
            }
            byte = (uint8_t)((byte << 1) | (bit ? 1 : 0));
        }

        bool eom = false;
        if (!read_bit(&eom)) {
            return false;
        }

        frame->bytes[frame->len++] = byte;

        const uint8_t destination = frame->bytes[0] & 0x0f;
        if (destination == own_logical_address && destination != CEC_LOGICAL_BROADCAST) {
            ack_after_initiator_edge();
        } else {
            skip_ack_after_initiator_edge();
        }

        if (eom) {
            frame->complete = true;
            return true;
        }
    }

    return false;
}

void cec_passive_reset(void) {
    cec_edge_read = cec_edge_write;
    passive_state = PASSIVE_WAIT_START_FALL;
    passive_frame.len = 0;
    passive_frame.complete = false;
    passive_byte = 0;
    passive_bit_index = 0;
    passive_expect_ack = false;
    passive_should_ack = false;
    passive_complete_after_ack = false;
}

static void passive_start_frame(void) {
    passive_frame.len = 0;
    passive_frame.complete = false;
    passive_byte = 0;
    passive_bit_index = 0;
    passive_expect_ack = false;
    passive_should_ack = false;
    passive_complete_after_ack = false;
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
    passive_should_ack = destination == own_logical_address && destination != CEC_LOGICAL_BROADCAST;
    passive_complete_after_ack = bit;
    return false;
}

static void passive_ack_slot(void) {
    size_t count = 0;
    uint32_t duration_us = 0;
    tx_append_symbol(cec_tx_words, &count, &duration_us, CEC_ZERO_LOW_US, CEC_BIT_TOTAL_US - CEC_ZERO_LOW_US);
    tx_start_symbols(cec_tx_words, count, duration_us);
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
                if (passive_accept_bit(low_us < 1050, frame)) {
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
