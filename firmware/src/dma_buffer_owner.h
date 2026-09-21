#ifndef PICOARC_DMA_BUFFER_OWNER_H
#define PICOARC_DMA_BUFFER_OWNER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DMA_BUFFER_FREE,
    DMA_BUFFER_BUILDING,
    DMA_BUFFER_READY,
    DMA_BUFFER_ACTIVE,
} dma_buffer_state_t;

static inline int dma_buffer_oldest_ready(
    const volatile dma_buffer_state_t state[2],
    const volatile uint32_t sequence[2]) {
    int selected = -1;
    for (unsigned int block = 0; block < 2; block++) {
        if (state[block] == DMA_BUFFER_READY &&
            (selected < 0 || sequence[block] < sequence[selected])) {
            selected = (int)block;
        }
    }
    return selected;
}

static inline int dma_buffer_acquire_free(
    volatile dma_buffer_state_t state[2]) {
    for (unsigned int block = 0; block < 2; block++) {
        if (state[block] == DMA_BUFFER_FREE) {
            state[block] = DMA_BUFFER_BUILDING;
            return (int)block;
        }
    }
    return -1;
}

static inline void dma_buffer_publish(volatile dma_buffer_state_t state[2],
                                      volatile uint32_t sequence[2],
                                      volatile uint32_t *next_sequence,
                                      unsigned int block) {
    sequence[block] = (*next_sequence)++;
    state[block] = DMA_BUFFER_READY;
}

static inline unsigned int dma_buffer_complete(
    volatile dma_buffer_state_t state[2],
    const volatile uint32_t sequence[2], unsigned int completed,
    bool *replayed) {
    const int ready = dma_buffer_oldest_ready(state, sequence);
    const unsigned int next = ready < 0 ? completed : (unsigned int)ready;
    *replayed = ready < 0;
    if (next != completed) state[completed] = DMA_BUFFER_FREE;
    state[next] = DMA_BUFFER_ACTIVE;
    return next;
}

#endif
