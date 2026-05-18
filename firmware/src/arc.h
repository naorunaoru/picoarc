#ifndef PICOARC_ARC_H
#define PICOARC_ARC_H

#include <stdbool.h>

void arc_init(unsigned int cec_pin, unsigned int hpd_pin);
void arc_task(void);
bool arc_is_initiated(void);

#endif
