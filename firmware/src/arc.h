#ifndef PICOARC_ARC_H
#define PICOARC_ARC_H

#include <stdbool.h>
#include <stdint.h>

void arc_init(unsigned int cec_pin, unsigned int hpd_pin);
void arc_task(void);
bool arc_is_initiated(void);
void arc_request_volume_sync(uint8_t volume);
void arc_request_mute_sync(bool muted);

#endif
