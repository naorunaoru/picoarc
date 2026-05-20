# PicoARC

PicoARC is a device that appears to the host as a USB Audio Class 2 audio interface and passes through incoming audio as an S/PDIF-style stream to an ARC-compatible device. In simpler terms, this allows you to output audio from your computer to a soundbar or an ARC-capable receiver.

It also allows you to control the soundbar's volume. Somewhat. Because apparently supporting absolute volume control over CEC is not something that vendors are eager to implement.

Did I tell you it does HDMI-CEC too? Yes, it does, because it's required for ARC negotiation.

## Why?

There's no off-the-shelf way of doing this exact thing, surprisingly. The closest thing is using S/PDIF, which has limited bandwidth, sometimes incurs additional latency and definitely does not allow you to issue any commands to the soundbar.

## Supported audio formats

- PCM at 32/44.1/48/88.2/96 kHz (higher sample rates are out of the question because RP2040 only has USB Full Speed)
- 16/20/24-bit
- IEC 61937 Dolby Digital/DTS passthrough

## Hardware

For now the hardware part exists as a hand-wired abomination. If you want to replicate it at this stage, you'll need:

- Pi Pico dev board
- HDMI breakout
- Few passives you probably have laying around

```
Net CEC_BUS
  Pico 3V3 (pin 36)  — R1 (27 kΩ) — node CEC_BUS
  node CEC_BUS                    — Pico GP3 (pin 5)
  node CEC_BUS                    — HDMI pin 13

Net ARC_TX
  Pico GP2 (pin 4)   — R2 (330 Ω) — node ATT_A
  node ATT_A         — R3 (56 Ω)  — Pico GND
  node ATT_A         — C1 (10 nF) — HDMI pin 14

Net HDMI_5V_FROM_SOUNDBAR
  HDMI pin 18        — R5 (1 kΩ)  — HDMI pin 19
  HDMI pin 18        — R6 (68 kΩ) — node HDMI_5V_SENSE
  node HDMI_5V_SENSE              — Pico GP4 (pin 6)
  node HDMI_5V_SENSE — R7 (100 kΩ) — Pico GND

Net GND
  Pico GND (pin 38)               — HDMI pin 17
```

## Build requirements

- Raspberry Pi Pico SDK
- CMake and Ninja
- Arm GNU toolchain
- `picotool`

The build files look for the Pico SDK at `~/Developer/pico/pico-sdk` by default.
If your SDK is elsewhere, set `PICO_SDK_PATH` before building.

## Build

From this directory:

```sh
make build
```

This configures and builds both firmware variants:

```text
firmware/build-release/picoarc-release.uf2
firmware/build-debug/picoarc-debug.uf2
```

The release variant is audio-only with logging compiled out. It waits for an
HDMI ARC device, completes ARC/SAD capability discovery, and then enumerates as
USB audio. The debug variant keeps the USB serial log and reset interface online
for bring-up work, so it enumerates immediately with the default audio
descriptor.

## Flash

Connect the Pico over USB, then run:

```sh
make flash
```

The helper script builds the release firmware first, then uses `picotool` to
load and run the UF2. If the Pico is not detected, hold BOOTSEL while plugging
it in and run the command again.

## Monitor

To open the USB serial log:

```sh
make monitor
```

To build, flash, and then open the monitor in one step:

```sh
make run
```

This flashes the debug variant, because the release variant does not expose a
USB serial log.

Press `Ctrl-]` to close the monitor.
