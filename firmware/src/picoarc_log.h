#ifndef PICOARC_LOG_H
#define PICOARC_LOG_H

#ifndef PICOARC_LOGGING
#define PICOARC_LOGGING 1
#endif

#if PICOARC_LOGGING
#include <stdarg.h>

void picoarc_log_init(void);
void picoarc_log_task(void);
int picoarc_printf(const char *format, ...);
int picoarc_puts(const char *text);
int picoarc_putchar(int character);

#ifndef PICOARC_LOG_IMPLEMENTATION
#define printf(...) picoarc_printf(__VA_ARGS__)
#define puts(...) picoarc_puts(__VA_ARGS__)
#define putchar(...) picoarc_putchar(__VA_ARGS__)
#endif
#else
#define picoarc_log_init() ((void)0)
#define picoarc_log_task() ((void)0)
#define printf(...) ((void)0)
#define puts(...) ((void)0)
#define putchar(...) ((void)0)
#endif

#endif
