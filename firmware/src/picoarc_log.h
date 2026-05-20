#ifndef PICOARC_LOG_H
#define PICOARC_LOG_H

#ifndef PICOARC_LOGGING
#define PICOARC_LOGGING 1
#endif

#if PICOARC_LOGGING
#include <stdio.h>
#else
#define printf(...) ((void)0)
#define puts(...) ((void)0)
#endif

#endif
