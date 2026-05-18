#include "cec.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "cec_rx.pio.h"
#include "cec_tx.pio.h"
#include "pico/stdlib.h"

enum {
    CEC_IDLE_BEFORE_TX_US = 5000,
    CEC_ACK_TIMEOUT_US = 6000,
    CEC_RX_DMA_WORDS = 1024,
    CEC_RX_DMA_RING_BITS = 12,
    CEC_TX_COMMANDS_PER_SEGMENT = 1 + 9 + 1,
};

enum {
    TX_CMD_ZERO = cec_tx_CEC_TX_CMD_ZERO,
    TX_CMD_ONE = cec_tx_CEC_TX_CMD_ONE,
    TX_CMD_START = cec_tx_CEC_TX_CMD_START,
    TX_CMD_ACK_SAMPLE = cec_tx_CEC_TX_CMD_ACK_SAMPLE,
    TX_CMD_DONE = cec_tx_CEC_TX_CMD_DONE,
};

static unsigned int cec_pin;
static PIO cec_tx_pio;
static unsigned int cec_tx_sm;
static unsigned int cec_tx_dma_chan;
static dma_channel_config cec_tx_dma_config;
static PIO cec_rx_pio;
static unsigned int cec_rx_sm;
static unsigned int cec_rx_dma_chan;
static dma_channel_config cec_rx_dma_config;
static bool cec_tx_active;
static uint8_t own_logical_address = CEC_LOGICAL_TV;
static volatile uint32_t cec_rx_words[CEC_RX_DMA_WORDS] __attribute__((aligned(1u << CEC_RX_DMA_RING_BITS)));
static volatile unsigned int cec_rx_read;

static cec_frame_t passive_frame;
static uint32_t cec_tx_commands[CEC_TX_COMMANDS_PER_SEGMENT];

static uint64_t now_us(void) {
    return time_us_64();
}

static void release_bus(void) {
    gpio_set_dir(cec_pin, GPIO_IN);
}

static void tx_append_command(uint32_t *commands, size_t *count, uint32_t command) {
    commands[(*count)++] = command;
}

static void tx_append_bit(uint32_t *commands, size_t *count, bool value) {
    tx_append_command(commands, count, value ? TX_CMD_ONE : TX_CMD_ZERO);
}

static void tx_service(void) {
    if (cec_tx_active && pio_interrupt_get(cec_tx_pio, 0)) {
        pio_interrupt_clear(cec_tx_pio, 0);
        cec_tx_active = false;
    }
}

static void tx_wait_idle(void) {
    while (cec_tx_active) {
        tx_service();
        tight_loop_contents();
    }
}

static void tx_start_commands(uint32_t *commands, size_t count) {
    if (count == 0) {
        return;
    }

    tx_wait_idle();
    commands[count++] = TX_CMD_DONE;
    pio_sm_set_enabled(cec_tx_pio, cec_tx_sm, false);
    pio_sm_clear_fifos(cec_tx_pio, cec_tx_sm);
    pio_sm_restart(cec_tx_pio, cec_tx_sm);
    pio_interrupt_clear(cec_tx_pio, 0);
    cec_tx_active = true;
    pio_sm_set_enabled(cec_tx_pio, cec_tx_sm, true);

    dma_channel_configure(cec_tx_dma_chan,
                          &cec_tx_dma_config,
                          &cec_tx_pio->txf[cec_tx_sm],
                          commands,
                          count,
                          true);
}

static void tx_run_commands(uint32_t *commands, size_t count) {
    tx_start_commands(commands, count);
    tx_wait_idle();
}

static unsigned int rx_dma_write_index(void) {
    const uintptr_t base = (uintptr_t)cec_rx_words;
    const uintptr_t write = dma_channel_hw_addr(cec_rx_dma_chan)->write_addr;
    return (unsigned int)(((write - base) / sizeof(cec_rx_words[0])) & (CEC_RX_DMA_WORDS - 1));
}

static void rx_reset_read(void) {
    cec_rx_read = rx_dma_write_index();
}

static bool pop_rx_word(uint32_t *word) {
    const unsigned int read = cec_rx_read;
    if (read == rx_dma_write_index()) {
        return false;
    }

    *word = cec_rx_words[read];
    cec_rx_read = (read + 1) & (CEC_RX_DMA_WORDS - 1);
    return true;
}

static bool wait_idle(uint32_t idle_us, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;
    uint64_t high_since = gpio_get(cec_pin) ? now_us() : 0;

    rx_reset_read();

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

static bool send_ack_slot(void) {
    size_t count = 0;

    tx_append_command(cec_tx_commands, &count, TX_CMD_ACK_SAMPLE);
    tx_start_commands(cec_tx_commands, count);

    const uint64_t deadline = now_us() + CEC_ACK_TIMEOUT_US;
    while (now_us() < deadline) {
        tx_service();
        if (!pio_sm_is_rx_fifo_empty(cec_tx_pio, cec_tx_sm)) {
            const bool released = (pio_sm_get(cec_tx_pio, cec_tx_sm) & 1u) != 0;
            tx_wait_idle();
            return !released;
        }
        tight_loop_contents();
    }

    tx_wait_idle();
    return false;
}

void cec_init(unsigned int pin) {
    cec_pin = pin;
    gpio_init(cec_pin);
    gpio_put(cec_pin, 0);
    release_bus();

    cec_rx_pio = pio1;
    cec_rx_sm = pio_claim_unused_sm(cec_rx_pio, true);
    const unsigned int rx_offset = pio_add_program(cec_rx_pio, &cec_rx_program);

    pio_sm_config rx_config = cec_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_config, cec_pin);
    sm_config_set_jmp_pin(&rx_config, cec_pin);
    sm_config_set_in_shift(&rx_config, false, false, 32);
    sm_config_set_fifo_join(&rx_config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&rx_config, (float)clock_get_hz(clk_sys) / cec_rx_CEC_RX_RATE_HZ);
    pio_sm_init(cec_rx_pio, cec_rx_sm, rx_offset, &rx_config);
    pio_sm_set_enabled(cec_rx_pio, cec_rx_sm, true);

    cec_rx_dma_chan = dma_claim_unused_channel(true);
    cec_rx_dma_config = dma_channel_get_default_config(cec_rx_dma_chan);
    channel_config_set_transfer_data_size(&cec_rx_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&cec_rx_dma_config, false);
    channel_config_set_write_increment(&cec_rx_dma_config, true);
    channel_config_set_ring(&cec_rx_dma_config, true, CEC_RX_DMA_RING_BITS);
    channel_config_set_dreq(&cec_rx_dma_config, pio_get_dreq(cec_rx_pio, cec_rx_sm, false));
    dma_channel_configure(cec_rx_dma_chan,
                          &cec_rx_dma_config,
                          cec_rx_words,
                          &cec_rx_pio->rxf[cec_rx_sm],
                          0xffffffffu,
                          true);
    rx_reset_read();

    // RX decode consumes all 32 PIO1 instruction slots. TX shares PIO0 with
    // the one-instruction SPDIF program and owns the CEC pin output mux.
    cec_tx_pio = pio0;
    cec_tx_sm = pio_claim_unused_sm(cec_tx_pio, true);
    const unsigned int offset = pio_add_program(cec_tx_pio, &cec_tx_program);

    pio_sm_config config = cec_tx_program_get_default_config(offset);
    sm_config_set_set_pins(&config, cec_pin, 1);
    sm_config_set_in_pins(&config, cec_pin);
    sm_config_set_in_shift(&config, false, false, 32);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / cec_tx_CEC_TX_RATE_HZ);
    pio_sm_init(cec_tx_pio, cec_tx_sm, offset, &config);
    pio_gpio_init(cec_tx_pio, cec_pin);
    pio_sm_set_pins(cec_tx_pio, cec_tx_sm, 0);
    pio_sm_set_consecutive_pindirs(cec_tx_pio, cec_tx_sm, cec_pin, 1, false);

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

    const bool broadcast = (bytes[0] & 0x0f) == CEC_LOGICAL_BROADCAST;
    bool all_acked = true;
    for (size_t i = 0; i < len; i++) {
        size_t count = 0;
        if (i == 0) {
            tx_append_command(cec_tx_commands, &count, TX_CMD_START);
        }
        for (int bit = 7; bit >= 0; bit--) {
            tx_append_bit(cec_tx_commands, &count, (bytes[i] >> bit) & 1u);
        }

        const bool eom = i == len - 1;
        tx_append_bit(cec_tx_commands, &count, eom);
        tx_run_commands(cec_tx_commands, count);
        const bool acked = send_ack_slot();
        all_acked = all_acked && (broadcast ? !acked : acked);
    }

    cec_passive_reset();
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
    rx_reset_read();
    passive_frame.len = 0;
    passive_frame.complete = false;
}

static void passive_ack_slot(void) {
    size_t count = 0;
    tx_append_bit(cec_tx_commands, &count, false);
    tx_start_commands(cec_tx_commands, count);
}

bool cec_receive_frame_passive(cec_frame_t *frame) {
    uint32_t word = 0;

    tx_service();

    while (pop_rx_word(&word)) {
        if (passive_frame.len >= sizeof(passive_frame.bytes)) {
            cec_passive_reset();
            return false;
        }

        const uint8_t byte = (uint8_t)((word >> 1) & 0xffu);
        const bool eom = (word & 1u) != 0;
        passive_frame.bytes[passive_frame.len++] = byte;

        const uint8_t destination = passive_frame.bytes[0] & 0x0f;
        if (destination == own_logical_address && destination != CEC_LOGICAL_BROADCAST) {
            passive_ack_slot();
        }

        if (eom) {
            passive_frame.complete = true;
            *frame = passive_frame;
            passive_frame.len = 0;
            passive_frame.complete = false;
            return true;
        }
    }

    return false;
}
