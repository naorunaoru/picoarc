#define PICOARC_LOG_IMPLEMENTATION
#include "picoarc_log.h"

#if PICOARC_LOGGING

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/critical_section.h"
#include "tusb.h"

enum {
    LOG_RING_SIZE = 8192,
    LOG_FORMAT_BUFFER_SIZE = 256,
};

static critical_section_t log_lock;
static char log_ring[LOG_RING_SIZE];
static unsigned int log_read;
static unsigned int log_write;
static bool log_last_was_cr;

void picoarc_log_init(void) {
    critical_section_init(&log_lock);
    log_read = 0;
    log_write = 0;
    log_last_was_cr = false;
}

static unsigned int ring_free(void) {
    if (log_write >= log_read) {
        return LOG_RING_SIZE - (log_write - log_read) - 1;
    }
    return log_read - log_write - 1;
}

static void enqueue(const char *text, unsigned int length) {
    critical_section_enter_blocking(&log_lock);
    unsigned int free = ring_free();

    for (unsigned int i = 0; i < length; i++) {
        const char character = text[i];
        const bool add_cr = character == '\n' && !log_last_was_cr;
        const unsigned int needed = add_cr ? 2 : 1;
        if (free < needed) {
            break;
        }

        if (add_cr) {
            log_ring[log_write] = '\r';
            log_write = (log_write + 1) % LOG_RING_SIZE;
            free--;
        }
        log_ring[log_write] = character;
        log_write = (log_write + 1) % LOG_RING_SIZE;
        free--;
        log_last_was_cr = character == '\r';
    }
    critical_section_exit(&log_lock);
}

int picoarc_printf(const char *format, ...) {
    char buffer[LOG_FORMAT_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    const int result = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (result > 0) {
        const unsigned int length = result < (int)sizeof(buffer) ?
                                        (unsigned int)result :
                                        sizeof(buffer) - 1;
        enqueue(buffer, length);
    }
    return result;
}

int picoarc_puts(const char *text) {
    const unsigned int length = (unsigned int)strlen(text);
    enqueue(text, length);
    enqueue("\n", 1);
    return (int)(length + 1);
}

int picoarc_putchar(int character) {
    const char value = (char)character;
    enqueue(&value, 1);
    return character;
}

void picoarc_log_task(void) {
    if (!tud_cdc_connected()) {
        return;
    }

    bool needs_flush = false;
    uint32_t available;
    while ((available = tud_cdc_write_available()) != 0) {
        char chunk[64];
        unsigned int length = 0;
        if (available > sizeof(chunk)) {
            available = sizeof(chunk);
        }

        critical_section_enter_blocking(&log_lock);
        unsigned int cursor = log_read;
        while (cursor != log_write && length < available) {
            chunk[length++] = log_ring[cursor];
            cursor = (cursor + 1) % LOG_RING_SIZE;
        }
        critical_section_exit(&log_lock);

        if (!length) {
            break;
        }

        const uint32_t written = tud_cdc_write(chunk, length);
        critical_section_enter_blocking(&log_lock);
        for (uint32_t i = 0; i < written; i++) {
            log_read = (log_read + 1) % LOG_RING_SIZE;
        }
        critical_section_exit(&log_lock);
        needs_flush |= written != 0;
        if (written < length) {
            break;
        }
    }

    if (needs_flush) {
        tud_cdc_write_flush();
    }
}

#endif
