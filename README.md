# PicoARC

PicoARC is a device that appears to the host as a USB Audio Class 2 audio interface and passes through incoming audio as an S/PDIF-style stream to an ARC-compatible device. In simpler terms, this allows you to output audio from your computer to a soundbar or an ARC-capable receiver.

It also allows you to control the soundbar's volume. Somewhat. Because apparently supporting absolute volume control over CEC is not something that vendors are eager to implement.

Did I tell you it does HDMI-CEC too? Yes, it does, because it's required for ARC negotiation.

## Why?

There's no off-the-shelf way of doing this exact thing, surprisingly. The closest thing is using S/PDIF, which has limited bandwidth, sometimes incurs additional latency and definitely does not allow you to control the soundbar from the host.

## How does it work?

On the USB side, it acts as an audio interface, grabs the audio stream and passes it through.

On the HDMI side, it pretends to be a TV: responds to DDC queries with a plausible EDID, negotiates ARC with the connected audio system over CEC and drives the ARC line within the constraints defined by the HDMI 1.4 spec.

## Supported audio formats

- PCM at 32/44.1/48/88.2/96 kHz (higher sample rates are out of the question because RP2040 only has USB Full Speed)
- 16/20/24-bit
- IEC 61937 Dolby Digital/DTS passthrough

## Hardware

The PCB design lives under `hardware/`. The schematic source is written in
[Zener](https://docs.pcb.new/pages/spec), so `hardware/picoarc.zen` is the
schematic/netlist source of truth rather than a `.kicad_sch`; the KiCad layout
and JLCPCB production files are checked in alongside it.

See [hardware/README.md](hardware/README.md) for the hardware file map,
Zener/KiCad workflow, and the old hand-wired Pico prototype wiring.

## License

PicoARC uses a split license. Hardware design materials are licensed under
`CERN-OHL-W-2.0`, and firmware plus general project documentation are licensed
under `MIT`. See [LICENSE](LICENSE) for the exact scope and full license text
locations.

## Build requirements

- Raspberry Pi Pico SDK
- CMake and Ninja
- Arm GNU toolchain
- `picotool`

The build files look for the Pico SDK at `~/Developer/pico/pico-sdk` by default.
If your SDK is elsewhere, set `PICO_SDK_PATH` before building.
TinyUSB 0.21.0 is bundled under `firmware/third_party/tinyusb`; builds do not
use the TinyUSB revision from the installed Pico SDK.

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

By default PicoARC keeps the ARC/S/PDIF carrier alive with silence while USB
audio is idle, which helps short notification sounds avoid being swallowed while
the soundbar wakes. To build firmware that lets the soundbar go to standby when
USB audio is idle, edit `firmware/src/picoarc_config.h` before building:

```c
#define PICOARC_IDLE_AUDIO_KEEPALIVE 0
```

Set it back to `1` to keep the default silence keepalive behavior.

The release variant is audio-only with logging compiled out. It waits for an
HDMI ARC device, completes ARC/SAD capability discovery, asks the soundbar for
its OSD name, and then enumerates as USB audio using that name when the soundbar
provides one. The debug variant keeps the USB serial log and reset interface
online for bring-up work, so it enumerates immediately with the default audio
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
