#include "cec_status_mailbox.h"
#include "dma_buffer_owner.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static unsigned int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void check_dma_ownership(void) {
    volatile dma_buffer_state_t state[2] = {
        DMA_BUFFER_ACTIVE, DMA_BUFFER_BUILDING
    };
    volatile uint32_t sequence[2] = {4, 5};

    CHECK(dma_buffer_oldest_ready(state, sequence) == -1);
    CHECK(state[1] == DMA_BUFFER_BUILDING);

    state[1] = DMA_BUFFER_READY;
    CHECK(dma_buffer_oldest_ready(state, sequence) == 1);
    state[0] = DMA_BUFFER_READY;
    sequence[0] = 6;
    CHECK(dma_buffer_oldest_ready(state, sequence) == 1);
    sequence[0] = 3;
    CHECK(dma_buffer_oldest_ready(state, sequence) == 0);

    bool replayed = true;
    unsigned int active = dma_buffer_complete(state, sequence, 1, &replayed);
    CHECK(!replayed && active == 0);
    CHECK(state[1] == DMA_BUFFER_FREE && state[0] == DMA_BUFFER_ACTIVE);
    const int building = dma_buffer_acquire_free(state);
    CHECK(building == 1 && state[1] == DMA_BUFFER_BUILDING);
    uint32_t next_sequence = 7;
    dma_buffer_publish(state, sequence, &next_sequence, 1);
    CHECK(state[1] == DMA_BUFFER_READY && sequence[1] == 7);
    active = dma_buffer_complete(state, sequence, 0, &replayed);
    CHECK(!replayed && active == 1 && state[0] == DMA_BUFFER_FREE);

    state[0] = DMA_BUFFER_ACTIVE;
    state[1] = DMA_BUFFER_BUILDING;
    for (unsigned int miss = 0; miss < 1000; miss++) {
        active = dma_buffer_complete(state, sequence, 0, &replayed);
        CHECK(replayed && active == 0);
        CHECK(state[0] == DMA_BUFFER_ACTIVE);
        CHECK(state[1] == DMA_BUFFER_BUILDING);
    }
}

static void check_cec_chronology(void) {
    cec_status_mailbox_t cec = {0};
    cec_status_mailbox_t taken = {0};

    cec_status_post_mute(&cec, true, false);
    cec_status_post_full(&cec, 42, false, true);
    CHECK(cec.pending && cec.volume_valid && cec.volume == 42);
    CHECK(!cec.muted && cec.notify_host);
    CHECK(cec_status_take(&cec, &taken));
    CHECK(taken.volume_valid && taken.volume == 42 && !taken.muted);

    cec_status_post_mute(&cec, true, false);
    CHECK(cec_status_take(&cec, &taken));
    CHECK(!taken.volume_valid && taken.muted);

    cec_status_post_full(&cec, 67, false, false);
    cec_status_post_mute(&cec, true, true);
    CHECK(cec_status_take(&cec, &taken));
    CHECK(taken.volume_valid && taken.volume == 67 && taken.muted);
    CHECK(taken.notify_host);

    cec_status_post_full(&cec, 10, true, false);
    cec_status_post_full(&cec, 11, false, false);
    CHECK(cec_status_take(&cec, &taken));
    CHECK(taken.volume == 11 && !taken.muted);
}

int main(void) {
    check_dma_ownership();
    check_cec_chronology();
    if (failures != 0) {
        fprintf(stderr, "%u check(s) failed\n", failures);
        return 1;
    }
    puts("core1_safety_test: all checks passed");
    return 0;
}
