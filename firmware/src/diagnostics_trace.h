#ifndef PICOARC_DIAGNOSTICS_TRACE_H
#define PICOARC_DIAGNOSTICS_TRACE_H

#include <stddef.h>
#include <stdint.h>

#if PICOARC_USB_DIAGNOSTICS
enum {
    DIAGNOSTICS_TRACE_CAPACITY = 128,
    DIAGNOSTICS_TRACE_STATE = 1,
    DIAGNOSTICS_TRACE_CEC_TX = 2,
    DIAGNOSTICS_TRACE_CEC_RX = 3,
    DIAGNOSTICS_TRACE_RECOVERY = 4,
    DIAGNOSTICS_TRACE_USB_ALT = 5,
};

typedef struct {
    uint32_t sequence;
    uint32_t uptime_ms;
    uint16_t kind;
    uint16_t flags;
    uint32_t value;
    uint8_t bytes[16];
} diagnostics_trace_entry_t;

typedef struct {
    uint32_t version;
    uint32_t uptime_ms;
    uint32_t total_entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t entry_size;
    uint32_t reserved[2];
    diagnostics_trace_entry_t entries[DIAGNOSTICS_TRACE_CAPACITY];
} diagnostics_trace_snapshot_t;

// All writers and readers run on core 0 in task context, never from an ISR.
// Reads retain history; only a processor restart clears it. Counters wrap.
void diagnostics_trace_record(uint32_t uptime_ms, uint16_t kind, uint16_t flags,
                              uint32_t value, const uint8_t *bytes, size_t len);
void diagnostics_trace_snapshot(uint32_t uptime_ms, diagnostics_trace_snapshot_t *snapshot);
#endif

#endif
