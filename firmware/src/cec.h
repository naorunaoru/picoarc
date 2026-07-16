#ifndef PICOARC_CEC_H
#define PICOARC_CEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CEC_LOGICAL_TV = 0x0,
    CEC_LOGICAL_AUDIO_SYSTEM = 0x5,
    CEC_LOGICAL_BROADCAST = 0xf,
};

typedef struct {
    uint8_t bytes[16];
    size_t len;
    bool complete;
} cec_frame_t;

typedef void (*cec_yield_fn)(void);

void cec_init(unsigned int pin);
void cec_set_yield(cec_yield_fn fn);
void cec_delay_ms(uint32_t delay_ms);
bool cec_bus_is_high(void);
void cec_set_logical_address(uint8_t logical_address);
bool cec_send_frame(const uint8_t *bytes, size_t len);
bool cec_send_with_retry(const uint8_t *bytes, size_t len, unsigned int attempts);
bool cec_poll(uint8_t initiator, uint8_t follower);
bool cec_receive_frame(cec_frame_t *frame, uint32_t timeout_us);
bool cec_receive_frame_passive(cec_frame_t *frame);
void cec_passive_reset(void);
void cec_reset_receiver(void);

#endif
