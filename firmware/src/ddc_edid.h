#ifndef PICOARC_DDC_EDID_H
#define PICOARC_DDC_EDID_H

#include <stdbool.h>

void ddc_edid_init(unsigned int sda_pin, unsigned int scl_pin);
void ddc_edid_note_hotplug(void);
bool ddc_edid_ready_for_cec(void);
void ddc_edid_task(void);

#endif
