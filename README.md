<img width="1400" height="788" alt="01-clean-isometric" src="https://github.com/user-attachments/assets/b9dece9f-7460-4226-a94b-06d028a8eb8d" />

# PicoARC

PicoARC is a device that appears to the host as a USB audio interface and passes through incoming audio as an S/PDIF-style stream to an ARC-compatible device. In simpler terms, this allows you to output audio from your computer to a soundbar or an ARC-capable receiver. It builds as USB Audio Class 1 by default for broad host compatibility; USB Audio Class 2 remains available as an opt-in target.

It also allows you to control the soundbar's volume. Somewhat. Because apparently supporting absolute volume control over CEC is not something that vendors are eager to implement.

Host volume and mute changes propagate to the soundbar in both USB modes. The
latest soundbar state is returned by subsequent USB control queries. UAC2 also
sends AudioControl change notifications; UAC1 has no notification endpoint, so
host volume controls generally remain stale after an on-device adjustment.
Host interfaces may ignore the UAC2 notifications as well.

Did I tell you it does HDMI-CEC too? Yes, it does, because it's required for ARC negotiation.

## Why?

There's no off-the-shelf way of doing this exact thing, surprisingly. The closest thing is using S/PDIF, which has limited bandwidth, sometimes incurs additional latency and definitely does not allow you to control the soundbar from the host.

## How does it work?

On the USB side, it acts as an audio interface, grabs the audio stream and passes it through.

On the HDMI side, it pretends to be a TV: responds to DDC queries with a plausible EDID, negotiates ARC with the connected audio system over CEC and drives the ARC line within the constraints defined by the HDMI 1.4 spec.

## Supported audio formats

- PCM at 32/44.1/48/88.2/96 kHz (higher sample rates are out of the question because RP2040 only has USB Full Speed)
- 16/20/24-bit

The optional UAC2 target additionally exposes separate IEC 61937 Dolby Digital
(AC-3) and DTS passthrough alternates. They are omitted from the default UAC1
target because host-driver support for Type III transport is inconsistent.

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

- Git
- CMake and Ninja
- Arm GNU toolchain
- `picotool`

The Pico SDK and TinyUSB sources are pinned as git submodules under
`firmware/third_party`; builds do not use SDK copies installed on the host.
PicoARC uses Pico SDK 2.2.0 and upstream TinyUSB 0.21.0.

## Build

From this directory:

```sh
git submodule update --init --recursive
make build
```

This configures and builds both firmware variants:

```text
firmware/build-release/picoarc-release.uf2
firmware/build-debug/picoarc-debug.uf2
```

Those are UAC1 builds. To build the opt-in UAC2 target instead:

```sh
make build-uac2
```

Its artifacts use separate build directories and names so CMake and host USB
caches cannot mix the two personalities:

```text
firmware/build-release-uac2/picoarc-release-uac2.uf2
firmware/build-debug-uac2/picoarc-debug-uac2.uf2
```

UAC1 is the recommended full-speed target and is the mode tested for Windows
compatibility. Treat UAC2 as experimental on full-speed hosts; it is retained
for IEC 61937 and uses an adaptive OUT endpoint without explicit feedback. The
firmware follows the host's delivery rate by servoing the S/PDIF PIO clock from
the receive-buffer level.

The equivalent helper option is `--usb-audio-class uac2` (or `--uac uac2`),
for example `./picoarc build release --uac uac2`. Direct CMake builds can set
`-DPICOARC_USB_AUDIO_CLASS=uac2`; omitting the option selects UAC1.

By default PicoARC stops the ARC/S/PDIF carrier while USB audio is idle so the
soundbar can go to standby. To keep sending silence instead, which can prevent
short notification sounds from being swallowed while the soundbar wakes, edit
`firmware/src/picoarc_config.h` before building:

```c
#define PICOARC_IDLE_AUDIO_KEEPALIVE 1
```

Set it back to `0` for the default standby-friendly behavior.

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

To flash the optional UAC2 release build, use `make flash-uac2`.

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

`make run-uac2` does the same with the optional UAC2 debug build.

Press `Ctrl-]` to close the monitor.
