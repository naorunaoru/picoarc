#include <assert.h>
#include <stdint.h>

#include "dma_buffer_owner.h"
#include "cec_status_mailbox.h"
#include "pcm_convert.h"

int main(void) {
    volatile dma_buffer_state_t state[2] = {
        DMA_BUFFER_ACTIVE, DMA_BUFFER_BUILDING
    };
    volatile uint32_t sequence[2] = { 4, 5 };

    // A completion during BUILDING has no READY source. Production therefore
    // replays the completed ACTIVE array; it never selects or mutates BUILDING.
    assert(dma_buffer_oldest_ready(state, sequence) == -1);
    assert(state[1] == DMA_BUFFER_BUILDING);

    state[1] = DMA_BUFFER_READY;
    assert(dma_buffer_oldest_ready(state, sequence) == 1);
    state[0] = DMA_BUFFER_READY;
    sequence[0] = 6;
    assert(dma_buffer_oldest_ready(state, sequence) == 1);
    sequence[0] = 3;
    assert(dma_buffer_oldest_ready(state, sequence) == 0);

    bool replayed;
    unsigned int active = dma_buffer_complete(state, sequence, 1, &replayed);
    assert(!replayed && active == 0);
    assert(state[1] == DMA_BUFFER_FREE && state[0] == DMA_BUFFER_ACTIVE);
    int building = dma_buffer_acquire_free(state);
    assert(building == 1 && state[1] == DMA_BUFFER_BUILDING);
    uint32_t next_sequence = 7;
    dma_buffer_publish(state, sequence, &next_sequence, 1);
    assert(state[1] == DMA_BUFFER_READY && sequence[1] == 7);
    active = dma_buffer_complete(state, sequence, 0, &replayed);
    assert(!replayed && active == 1 && state[0] == DMA_BUFFER_FREE);

    // Consecutive misses still select no unowned address. The ISR's only
    // fallback is the just-completed bounded source array.
    state[0] = DMA_BUFFER_ACTIVE;
    state[1] = DMA_BUFFER_BUILDING;
    for (unsigned int miss = 0; miss < 1000; miss++) {
        active = dma_buffer_complete(state, sequence, 0, &replayed);
        assert(replayed && active == 0);
        assert(state[0] == DMA_BUFFER_ACTIVE);
        assert(state[1] == DMA_BUFFER_BUILDING);
    }

    assert(pcm16_to_internal(INT16_MIN) == INT32_MIN);
    assert(pcm16_to_internal(-1) == -65536);
    assert(pcm16_to_internal(0) == 0);
    assert(pcm16_to_internal(INT16_MAX) == INT32_C(2147418112));

    cec_status_mailbox_t cec = {0};
    cec_status_post_mute(&cec, true, false);
    cec_status_post_full(&cec, 42, false, true);
    assert(cec.pending && cec.volume_valid && cec.volume == 42);
    assert(!cec.muted && cec.notify_host);

    cec_status_post_full(&cec, 67, false, false);
    cec_status_post_mute(&cec, true, true);
    assert(cec.volume_valid && cec.volume == 67 && cec.muted);
    assert(cec.notify_host);

    cec_status_post_full(&cec, 10, true, false);
    cec_status_post_full(&cec, 11, false, false);
    assert(cec.volume == 11 && !cec.muted);
    return 0;
}
