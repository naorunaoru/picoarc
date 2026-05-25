#include "ddc_edid.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/i2c_slave.h"
#include "pico/stdlib.h"
#include "picoarc_config.h"
#include "picoarc_log.h"

enum {
    DDC_EDID_ADDRESS = 0x50,
    DDC_EDID_SIZE = 256,
    DDC_EDID_BLOCK_SIZE = 128,
};

static const uint8_t edid[DDC_EDID_SIZE] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x41, 0xd0, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x24, 0x01, 0x04, 0x80, 0x10, 0x09, 0x78, 0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x50, 0x69, 0x63,
    0x6f, 0x41, 0x52, 0x43, 0x20, 0x54, 0x56, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x4b, 0x1e, 0x53, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x50, 0x49, 0x43, 0x4f, 0x41, 0x52, 0x43, 0x30, 0x30, 0x30, 0x31, 0x0a, 0x20, 0x01, 0x7c,
    0x02, 0x03, 0x15, 0x40, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x66, 0x03, 0x0c, 0x00,
    0x10, 0x00, 0x80, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x92,
};

static volatile uint8_t edid_offset;
static volatile uint8_t write_index;
static volatile uint32_t offset_writes;
static volatile uint32_t read_requests;
static volatile uint32_t bytes_sent;
static uint32_t cycle_start_ms;
static uint32_t cycle_start_bytes;
static uint32_t last_activity_ms;
static uint32_t last_offset_writes;
static uint32_t last_read_requests;
static uint32_t last_bytes_sent;
static uint32_t last_log_ms;

static uint8_t edid_block_checksum(unsigned int block) {
    uint8_t sum = 0;
    const unsigned int start = block * DDC_EDID_BLOCK_SIZE;
    for (unsigned int i = 0; i < DDC_EDID_BLOCK_SIZE; i++) {
        sum = (uint8_t)(sum + edid[start + i]);
    }
    return sum;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void ddc_edid_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE:
        while (i2c_get_read_available(i2c) > 0) {
            const uint8_t value = i2c_read_byte_raw(i2c);
            if (write_index == 0) {
                edid_offset = value;
                offset_writes++;
            }
            write_index++;
        }
        break;

    case I2C_SLAVE_REQUEST:
        if (i2c_get_write_available(i2c) > 0) {
            i2c_write_byte_raw(i2c, edid[edid_offset]);
            edid_offset++;
            bytes_sent++;
        }
        read_requests++;
        break;

    case I2C_SLAVE_FINISH:
        write_index = 0;
        break;
    }
}

void ddc_edid_init(unsigned int sda_pin, unsigned int scl_pin) {
#if PICOARC_DDC_EDID_ENABLE
    gpio_init(sda_pin);
    gpio_init(scl_pin);
    gpio_disable_pulls(sda_pin);
    gpio_disable_pulls(scl_pin);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);

    i2c_init(i2c1, 100000);
    i2c_slave_init(i2c1, DDC_EDID_ADDRESS, ddc_edid_handler);
    ddc_edid_note_hotplug();

    printf("ddc-edid: answering 0x%02x on GP%u SDA / GP%u SCL\n",
           DDC_EDID_ADDRESS,
           sda_pin,
           scl_pin);
    printf("ddc-edid: HDMI VSDB source physical address=1.0.0.0, checksums base=0x%02x cea=0x%02x\n",
           edid_block_checksum(0),
           edid_block_checksum(1));
#else
    (void)sda_pin;
    (void)scl_pin;
#endif
}

void ddc_edid_note_hotplug(void) {
#if PICOARC_DDC_EDID_ENABLE
    const uint32_t current_ms = now_ms();
    cycle_start_ms = current_ms;
    last_activity_ms = current_ms;
    cycle_start_bytes = bytes_sent;
#endif
}

bool ddc_edid_ready_for_cec(void) {
#if PICOARC_DDC_EDID_ENABLE
    const uint32_t current_ms = now_ms();
    const uint32_t cycle_bytes = bytes_sent - cycle_start_bytes;

    if (cycle_bytes >= PICOARC_DDC_EDID_CEC_READY_BYTES) {
        return current_ms - last_activity_ms >= PICOARC_DDC_EDID_CEC_SETTLE_MS;
    }

    return current_ms - cycle_start_ms >= PICOARC_DDC_EDID_CEC_NO_READ_TIMEOUT_MS;
#else
    return true;
#endif
}

void ddc_edid_task(void) {
#if PICOARC_DDC_EDID_ENABLE
    const uint32_t current_offset_writes = offset_writes;
    const uint32_t current_read_requests = read_requests;
    const uint32_t current_bytes_sent = bytes_sent;
    const uint32_t current_ms = now_ms();

    if (current_offset_writes == last_offset_writes &&
        current_read_requests == last_read_requests &&
        current_bytes_sent == last_bytes_sent) {
        return;
    }

    last_activity_ms = current_ms;
    if (current_ms - last_log_ms < 250 &&
        current_bytes_sent - last_bytes_sent < 64) {
        return;
    }

    printf("ddc-edid: offsets=%lu reads=%lu bytes=%lu next=0x%02x\n",
           (unsigned long)current_offset_writes,
           (unsigned long)current_read_requests,
           (unsigned long)current_bytes_sent,
           edid_offset);

    last_offset_writes = current_offset_writes;
    last_read_requests = current_read_requests;
    last_bytes_sent = current_bytes_sent;
    last_log_ms = current_ms;
#endif
}
