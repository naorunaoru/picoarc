#ifndef PICOARC_CONFIG_H
#define PICOARC_CONFIG_H

// Set to 1 to keep sending silence while USB audio is idle.
// Set to 0 to stop the ARC/S/PDIF carrier while idle so the soundbar can standby.
#define PICOARC_IDLE_AUDIO_KEEPALIVE 0

#if PICOARC_IDLE_AUDIO_KEEPALIVE
#define PICOARC_IDLE_AUDIO_POLICY "keepalive"
#else
#define PICOARC_IDLE_AUDIO_POLICY "standby"
#endif

#endif
