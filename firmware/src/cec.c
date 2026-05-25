#include "cec.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "cec_rx.pio.h"
#include "cec_tx.pio.h"
#include "pico/stdlib.h"

#include <stdio.h>

#include "picoarc_log.h"

enum {
    CEC_BIT_PERIOD_US = 2400,
    CEC_IDLE_BEFORE_TX_US = 5000,
    CEC_RX_DMA_WORDS = 1024,
    CEC_RX_DMA_RING_BITS = 12,

    CEC_MAX_FRAME_BYTES = 16,
    CEC_TX_WORDS_PER_BIT = 2,
    CEC_TX_WORDS_PER_ACK = 3,
    CEC_TX_WORDS_PER_BYTE = 9 * CEC_TX_WORDS_PER_BIT + CEC_TX_WORDS_PER_ACK,
    CEC_MAX_TX_WORDS = CEC_TX_WORDS_PER_BIT + CEC_MAX_FRAME_BYTES * CEC_TX_WORDS_PER_BYTE,
};

// Loop iteration counts derived from the published CEC bit timings together
// with the per-command dispatch overhead this PIO program incurs. Each value
// is "how many times the drive_loop / release_loop body runs before falling
// through", which equals (target_us minus a fixed overhead). The overhead is
// constant for the chained orderings the encoder produces below
// (drive_low -> release -> drive_low -> release; or drive_low -> release+sample
// -> release -> drive_low for an ACK slot), so the on-the-wire timing is
// independent of FIFO timing as long as DMA keeps the SM fed.
enum {
    CEC_LOOP_DRIVE_START = 3694,         // start-bit low: 3700 us
    CEC_LOOP_RELEASE_START = 794,        // start-bit high: 800 us
    CEC_LOOP_DRIVE_ZERO = 1494,          // 0-bit low: 1500 us
    CEC_LOOP_RELEASE_ZERO = 894,         // 0-bit high: 900 us
    CEC_LOOP_DRIVE_ONE = 594,            // 1-bit low: 600 us
    CEC_LOOP_RELEASE_ONE = 1794,         // 1-bit high: 1800 us
    CEC_LOOP_ACK_DRIVE = 594,            // ACK slot sender drive: 600 us low
    CEC_LOOP_ACK_RELEASE_SAMPLE = 448,   // sample at 450 us into release (1050 us after fall)
    CEC_LOOP_ACK_RELEASE_POST = 1336,    // remaining release to close the 2400 us bit period
};

static const bool cec_trace_tx = true;

static unsigned int cec_pin;
static PIO cec_pio;
static unsigned int cec_tx_sm;
static unsigned int cec_tx_offset;
static unsigned int cec_tx_dma_chan;
static unsigned int cec_ack_dma_chan;
static dma_channel_config cec_ack_dma_config;
static PIO cec_rx_pio;
static unsigned int cec_rx_sm;
static unsigned int cec_rx_dma_chan;
static dma_channel_config cec_tx_dma_config;
static dma_channel_config cec_rx_dma_config;

static cec_yield_fn cec_yield_fn_ptr;

static uint16_t own_logical_address_mask = 1u << CEC_LOGICAL_TV;
static cec_frame_t passive_frame;
static volatile uint32_t cec_rx_words[CEC_RX_DMA_WORDS] __attribute__((aligned(1u << CEC_RX_DMA_RING_BITS)));
static volatile unsigned int cec_rx_read;

static uint32_t cec_tx_words[CEC_MAX_TX_WORDS];
static uint32_t cec_rx_samples[CEC_MAX_FRAME_BYTES];

static uint64_t now_us(void) {
    return time_us_64();
}

static void cec_yield(void) {
    if (cec_yield_fn_ptr) {
        cec_yield_fn_ptr();
    }
}

void cec_set_yield(cec_yield_fn fn) {
    cec_yield_fn_ptr = fn;
}

void cec_delay_ms(uint32_t delay_ms) {
    const uint64_t deadline = now_us() + (uint64_t)delay_ms * 1000u;

    while (now_us() < deadline) {
        cec_yield();
        tight_loop_contents();
    }
}

static inline uint32_t encode_drive(uint32_t loop_iters) {
    const uint32_t x_init = loop_iters > 0 ? loop_iters - 1 : 0;
    return x_init << 1;
}

static inline uint32_t encode_release(uint32_t loop_iters, bool sample) {
    const uint32_t x_init = loop_iters > 0 ? loop_iters - 1 : 0;
    return (x_init << 2) | (sample ? 0b11u : 0b01u);
}

static size_t emit_start_bit(uint32_t *buf, size_t idx) {
    buf[idx++] = encode_drive(CEC_LOOP_DRIVE_START);
    buf[idx++] = encode_release(CEC_LOOP_RELEASE_START, false);
    return idx;
}

static size_t emit_bit(uint32_t *buf, size_t idx, bool value) {
    if (value) {
        buf[idx++] = encode_drive(CEC_LOOP_DRIVE_ONE);
        buf[idx++] = encode_release(CEC_LOOP_RELEASE_ONE, false);
    } else {
        buf[idx++] = encode_drive(CEC_LOOP_DRIVE_ZERO);
        buf[idx++] = encode_release(CEC_LOOP_RELEASE_ZERO, false);
    }
    return idx;
}

static size_t emit_ack_slot(uint32_t *buf, size_t idx) {
    buf[idx++] = encode_drive(CEC_LOOP_ACK_DRIVE);
    buf[idx++] = encode_release(CEC_LOOP_ACK_RELEASE_SAMPLE, true);
    buf[idx++] = encode_release(CEC_LOOP_ACK_RELEASE_POST, false);
    return idx;
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

static bool wait_bus_idle(uint32_t idle_us, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;
    uint64_t high_since = gpio_get(cec_pin) ? now_us() : 0;

    while (now_us() < deadline) {
        cec_yield();

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

static void tx_pin_to_pio(void) {
    gpio_set_function(cec_pin, GPIO_FUNC_PIO1);
}

static void tx_reset_sm(void) {
    pio_sm_set_enabled(cec_pio, cec_tx_sm, false);
    dma_channel_abort(cec_tx_dma_chan);
    dma_channel_abort(cec_ack_dma_chan);
    pio_sm_clear_fifos(cec_pio, cec_tx_sm);
    pio_sm_restart(cec_pio, cec_tx_sm);
    // The reset_entry label is placed immediately after `.wrap` in cec_tx.pio,
    // so its offset is `cec_tx_wrap + 1`. pioasm doesn't auto-export label
    // offsets, so we derive it here from the wrap constant the assembler does
    // emit. Running reset_entry forces pindirs to 0 and then jmps to top so
    // the bus is guaranteed released before the first DMA-fed action.
    pio_sm_exec(cec_pio, cec_tx_sm, pio_encode_jmp(cec_tx_offset + cec_tx_wrap + 1));
    pio_sm_set_enabled(cec_pio, cec_tx_sm, true);
}

static void tx_start_async(const uint32_t *words, size_t count) {
    tx_reset_sm();
    tx_pin_to_pio();

    dma_channel_configure(cec_tx_dma_chan,
                          &cec_tx_dma_config,
                          &cec_pio->txf[cec_tx_sm],
                          words,
                          count,
                          true);
}

static void tx_start_async_with_samples(const uint32_t *words, size_t count,
                                        uint32_t *samples, size_t sample_count) {
    tx_reset_sm();
    tx_pin_to_pio();

    if (sample_count > 0) {
        dma_channel_configure(cec_ack_dma_chan,
                              &cec_ack_dma_config,
                              samples,
                              &cec_pio->rxf[cec_tx_sm],
                              sample_count,
                              true);
    }

    dma_channel_configure(cec_tx_dma_chan,
                          &cec_tx_dma_config,
                          &cec_pio->txf[cec_tx_sm],
                          words,
                          count,
                          true);
}

static void tx_wait_done(void) {
    while (dma_channel_is_busy(cec_tx_dma_chan) || dma_channel_is_busy(cec_ack_dma_chan)) {
        cec_yield();
        tight_loop_contents();
    }

    // Let the SM finish the trailing release cycles after the last DMA word
    // before tearing it down. The longest trailing cmd is the ACK post-release
    // (~1.35 ms); we add a little slack so the line is back high at SIO swap.
    const uint64_t deadline = now_us() + 1500;
    while (now_us() < deadline) {
        cec_yield();
        tight_loop_contents();
    }
}

void cec_init(unsigned int pin) {
    cec_pin = pin;
    gpio_init(cec_pin);
    gpio_put(cec_pin, 0);
    gpio_set_dir(cec_pin, GPIO_IN);

    // PIO0 has room for the one-instruction S/PDIF program plus this passive
    // CEC RX decoder. RX only samples the pin, so it can observe GP3 even
    // while PIO1 owns the output mux for TX.
    cec_rx_pio = pio0;
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

    // Keep CEC TX on PIO1 so it does not share a PIO instance with S/PDIF.
    // Pre-set the pin output value to 0 so toggling pindirs alone is enough
    // to drive the open-drain CEC bus.
    cec_pio = pio1;
    cec_tx_sm = pio_claim_unused_sm(cec_pio, true);
    cec_tx_offset = pio_add_program(cec_pio, &cec_tx_program);

    pio_sm_config config = cec_tx_program_get_default_config(cec_tx_offset);
    sm_config_set_set_pins(&config, cec_pin, 1);
    sm_config_set_in_pins(&config, cec_pin);
    sm_config_set_out_shift(&config, true /* shift right */, false /* no autopull */, 32);
    sm_config_set_in_shift(&config, false /* shift left */, false /* no autopush */, 32);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / cec_tx_CEC_TX_RATE_HZ);
    pio_sm_init(cec_pio, cec_tx_sm, cec_tx_offset, &config);
    pio_gpio_init(cec_pio, cec_pin);
    pio_sm_set_pins(cec_pio, cec_tx_sm, 0);
    pio_sm_set_consecutive_pindirs(cec_pio, cec_tx_sm, cec_pin, 1, false);
    tx_pin_to_pio();

    cec_tx_dma_chan = dma_claim_unused_channel(true);
    cec_tx_dma_config = dma_channel_get_default_config(cec_tx_dma_chan);
    channel_config_set_transfer_data_size(&cec_tx_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&cec_tx_dma_config, true);
    channel_config_set_write_increment(&cec_tx_dma_config, false);
    channel_config_set_dreq(&cec_tx_dma_config, pio_get_dreq(cec_pio, cec_tx_sm, true));

    cec_ack_dma_chan = dma_claim_unused_channel(true);
    cec_ack_dma_config = dma_channel_get_default_config(cec_ack_dma_chan);
    channel_config_set_transfer_data_size(&cec_ack_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&cec_ack_dma_config, false);
    channel_config_set_write_increment(&cec_ack_dma_config, true);
    channel_config_set_dreq(&cec_ack_dma_config, pio_get_dreq(cec_pio, cec_tx_sm, false));
}

bool cec_bus_is_high(void) {
    return gpio_get(cec_pin);
}

void cec_set_logical_address(uint8_t logical_address) {
    own_logical_address_mask = 1u << (logical_address & 0x0f);
}

static void trace_send_result(const uint8_t *bytes, size_t len, bool local_directed,
                              bool broadcast, bool all_acked) {
    if (!cec_trace_tx || len == 0) {
        return;
    }

    const uint8_t header = bytes[0];
    printf("cec: tx +%llums %x->%x",
           (unsigned long long)(time_us_64() / 1000),
           header >> 4,
           header & 0x0f);
    for (size_t i = 1; i < len; i++) {
        printf(" %02x", bytes[i]);
    }
    printf(" result=%s mode=%s acks=",
           all_acked ? "ok" : "fail",
           broadcast ? "broadcast" : (local_directed ? "local" : "direct"));
    for (size_t i = 0; i < len; i++) {
        printf("%u", (cec_rx_samples[i] & 1u) == 0u ? 1u : 0u);
    }
    printf("\n");
}

static bool cec_send_frame_internal(const uint8_t *bytes, size_t len) {
    if (len == 0 || len > CEC_MAX_FRAME_BYTES) {
        return false;
    }

    if (!wait_bus_idle(CEC_IDLE_BEFORE_TX_US, 100000)) {
        return false;
    }

    size_t count = emit_start_bit(cec_tx_words, 0);
    for (size_t i = 0; i < len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            count = emit_bit(cec_tx_words, count, ((bytes[i] >> bit) & 1u) != 0u);
        }
        const bool eom = i == len - 1;
        count = emit_bit(cec_tx_words, count, eom);
        count = emit_ack_slot(cec_tx_words, count);
    }

    const bool broadcast = (bytes[0] & 0x0f) == CEC_LOGICAL_BROADCAST;
    const uint8_t source = bytes[0] >> 4;
    const uint8_t destination = bytes[0] & 0x0f;
    const bool local_directed = !broadcast &&
                                 (own_logical_address_mask & (1u << source)) != 0u &&
                                 (own_logical_address_mask & (1u << destination)) != 0u;

    for (size_t i = 0; i < len; i++) {
        cec_rx_samples[i] = 0xffffffffu;
    }

    tx_start_async_with_samples(cec_tx_words, count, cec_rx_samples, len);
    tx_wait_done();

    cec_passive_reset();

    bool all_acked = true;
    for (size_t i = 0; i < len; i++) {
        const bool low_at_sample = (cec_rx_samples[i] & 1u) == 0u;
        const bool acked = local_directed ? true : low_at_sample;
        all_acked = all_acked && (broadcast ? !acked : acked);
    }

    trace_send_result(bytes, len, local_directed, broadcast, all_acked);
    return all_acked;
}

bool cec_send_frame(const uint8_t *bytes, size_t len) {
    return cec_send_frame_internal(bytes, len);
}

bool cec_send_with_retry(const uint8_t *bytes, size_t len, unsigned int attempts) {
    for (unsigned int attempt = 0; attempt < attempts; attempt++) {
        if (cec_send_frame(bytes, len)) {
            return true;
        }
        cec_delay_ms(50);
    }

    return false;
}

bool cec_poll(uint8_t initiator, uint8_t follower) {
    const uint8_t header = (uint8_t)((initiator << 4) | (follower & 0x0f));
    return cec_send_frame_internal(&header, 1);
}

bool cec_receive_frame(cec_frame_t *frame, uint32_t timeout_us) {
    const uint64_t deadline = now_us() + timeout_us;

    cec_passive_reset();
    while (now_us() < deadline) {
        if (cec_receive_frame_passive(frame)) {
            return true;
        }
        cec_yield();
        tight_loop_contents();
    }

    return false;
}

void cec_passive_reset(void) {
    rx_reset_read();
    passive_frame = (cec_frame_t){0};
}

static void passive_ack_slot(void) {
    // Emit a single 0-bit which holds the line low for 1500 us then releases
    // for 900 us — the standard follower-ACK shape for the upcoming bit period.
    size_t count = emit_bit(cec_tx_words, 0, false);
    tx_start_async(cec_tx_words, count);
}

bool cec_receive_frame_passive(cec_frame_t *frame) {
    uint32_t word = 0;

    while (pop_rx_word(&word)) {
        if (passive_frame.len >= sizeof(passive_frame.bytes)) {
            cec_passive_reset();
            return false;
        }

        const uint8_t byte = (uint8_t)((word >> 1) & 0xffu);
        const bool eom = (word & 1u) != 0;
        passive_frame.bytes[passive_frame.len++] = byte;

        const uint8_t destination = passive_frame.bytes[0] & 0x0f;
        const bool should_ack = destination != CEC_LOGICAL_BROADCAST &&
                                (own_logical_address_mask & (1u << destination)) != 0u;
        if (should_ack) {
            passive_ack_slot();
        }

        if (eom) {
            passive_frame.complete = true;
            *frame = passive_frame;
            passive_frame = (cec_frame_t){0};
            return true;
        }
    }

    return false;
}
