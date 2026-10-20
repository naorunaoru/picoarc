#ifndef PICOARC_DDC_EDID_H
#define PICOARC_DDC_EDID_H

#include <stdbool.h>
#include <stdint.h>

void ddc_edid_init(unsigned int sda_pin, unsigned int scl_pin);
void ddc_edid_note_hotplug(void);
bool ddc_edid_ready_for_cec(void);
void ddc_edid_task(void);

#if PICOARC_USB_DIAGNOSTICS
typedef struct {
    uint32_t enable;
    uint32_t status;
    uint32_t raw_intr_stat;
    uint32_t tx_abrt_source;
    uint32_t rx_fifo_level;
    uint32_t tx_fifo_level;
    uint32_t offset_writes;
    uint32_t read_requests;
    uint32_t bytes_sent;
    uint32_t edid_offset;
    uint32_t write_index;
} ddc_edid_diagnostics_t;

void ddc_edid_get_diagnostics(ddc_edid_diagnostics_t *diagnostics);
#endif

#if PICOARC_USB_DIAGNOSTICS
void ddc_edid_restart(bool hotplug);
#endif

#endif
