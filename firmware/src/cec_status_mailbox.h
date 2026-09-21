#ifndef PICOARC_CEC_STATUS_MAILBOX_H
#define PICOARC_CEC_STATUS_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool pending;
    bool volume_valid;
    uint8_t volume;
    bool muted;
    bool notify_host;
} cec_status_mailbox_t;

static inline void cec_status_post_full(cec_status_mailbox_t *box,
                                        uint8_t volume, bool muted,
                                        bool notify_host) {
    box->pending = true;
    box->volume_valid = true;
    box->volume = volume;
    box->muted = muted;
    box->notify_host = notify_host;
}

static inline void cec_status_post_mute(cec_status_mailbox_t *box,
                                        bool muted, bool notify_host) {
    box->pending = true;
    box->muted = muted;
    box->notify_host |= notify_host;
}

static inline bool cec_status_take(cec_status_mailbox_t *box,
                                   cec_status_mailbox_t *snapshot) {
    if (!box->pending) return false;
    *snapshot = *box;
    box->pending = false;
    box->volume_valid = false;
    box->notify_host = false;
    return true;
}

#endif
