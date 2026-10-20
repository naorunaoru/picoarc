#include "diagnostics_trace.h"

#if PICOARC_USB_DIAGNOSTICS
#include <string.h>

_Static_assert(sizeof(diagnostics_trace_entry_t) == 32, "Trace entry wire size changed");
_Static_assert(sizeof(diagnostics_trace_snapshot_t) == 4128, "Trace snapshot wire size changed");

static diagnostics_trace_entry_t history[DIAGNOSTICS_TRACE_CAPACITY];
static uint32_t total_entries;
static unsigned int next_entry;
static unsigned int entry_count;

void diagnostics_trace_record(uint32_t uptime_ms, uint16_t kind, uint16_t flags,
                              uint32_t value, const uint8_t *bytes, size_t len) {
    diagnostics_trace_entry_t *entry = &history[next_entry];
    *entry = (diagnostics_trace_entry_t) {
        .sequence = ++total_entries,
        .uptime_ms = uptime_ms,
        .kind = kind,
        .flags = flags,
        .value = value,
    };
    if (bytes) {
        if (len > sizeof(entry->bytes)) {
            len = sizeof(entry->bytes);
        }
        memcpy(entry->bytes, bytes, len);
    }
    next_entry = (next_entry + 1) % DIAGNOSTICS_TRACE_CAPACITY;
    if (entry_count < DIAGNOSTICS_TRACE_CAPACITY) {
        entry_count++;
    }
}

void diagnostics_trace_snapshot(uint32_t uptime_ms, diagnostics_trace_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = 1;
    snapshot->uptime_ms = uptime_ms;
    snapshot->total_entries = total_entries;
    snapshot->count = entry_count;
    snapshot->capacity = DIAGNOSTICS_TRACE_CAPACITY;
    snapshot->entry_size = sizeof(diagnostics_trace_entry_t);
    unsigned int index = (next_entry + DIAGNOSTICS_TRACE_CAPACITY - entry_count) %
                         DIAGNOSTICS_TRACE_CAPACITY;
    for (unsigned int i = 0; i < entry_count; i++) {
        snapshot->entries[i] = history[index];
        index = (index + 1) % DIAGNOSTICS_TRACE_CAPACITY;
    }
}
#endif
