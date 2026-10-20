#include "diagnostics_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return EXIT_FAILURE; \
    } \
} while (0)

int main(void) {
    diagnostics_trace_snapshot_t first, second;
    diagnostics_trace_snapshot(10, &first);
    CHECK(first.version == 1 && first.count == 0 && first.total_entries == 0);
    CHECK(sizeof(first) == 4128 && first.entry_size == 32 && first.capacity == 128);

    const uint8_t payload[] = {0x50, 0xc0};
    diagnostics_trace_record(20, DIAGNOSTICS_TRACE_CEC_RX, 0, 2, payload, sizeof(payload));
    diagnostics_trace_snapshot(30, &first);
    diagnostics_trace_snapshot(30, &second);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    CHECK(first.count == 1 && first.entries[0].sequence == 1);
    CHECK(first.entries[0].uptime_ms == 20 && first.entries[0].bytes[0] == 0x50);
    CHECK(first.entries[0].bytes[2] == 0 && first.entries[1].sequence == 0);

    uint8_t long_payload[32];
    memset(long_payload, 0xaa, sizeof(long_payload));
    diagnostics_trace_record(40, DIAGNOSTICS_TRACE_CEC_TX, 1, 16, long_payload, sizeof(long_payload));
    for (unsigned int i = 0; i < 200; i++) {
        diagnostics_trace_record(100 + i, DIAGNOSTICS_TRACE_STATE, i, 96000, NULL, 0);
    }
    diagnostics_trace_snapshot(500, &first);
    CHECK(first.total_entries == 202 && first.count == 128);
    CHECK(first.entries[0].sequence == 75 && first.entries[127].sequence == 202);
    CHECK(first.entries[0].uptime_ms == 172 && first.entries[127].uptime_ms == 299);
    for (unsigned int i = 0; i < first.count; i++) {
        CHECK(first.entries[i].sequence == 75 + i);
        CHECK(first.entries[i].flags == 72 + i);
        CHECK(first.entries[i].value == 96000);
        for (unsigned int j = 0; j < 16; j++) {
            CHECK(first.entries[i].bytes[j] == 0);
        }
    }
    diagnostics_trace_snapshot(500, &second);
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    return EXIT_SUCCESS;
}
