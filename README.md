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
provides one. USB sample rates are limited to the connected soundbar's reported
capabilities. UAC1 lists rates separately for each PCM bit depth; UAC2 reports
the available rates on its shared clock and validates the selected format when
starting a stream. Unsupported bit depths and codecs are omitted. UAC2 rejects
unsupported rate requests; UAC1 rounds them to a supported rate and reports the
selected value through GET_CUR. When SAD discovery is unavailable, the existing
fallback remains 16-bit PCM at 48 kHz.

Capabilities are frozen for each USB connection and refreshed by re-enumerating
when they change. The initial rate is 48 kHz if supported, otherwise the nearest
supported rate. The debug variant initially exposes its serial log and reset
interface with no streaming alternates, then briefly reconnects USB when audio
capabilities become available or change.

## Host sanitizer tests

The host tests require CMake, Ninja, and a native Clang or GCC installation
with AddressSanitizer and UndefinedBehaviorSanitizer runtimes. They do not need
the Pico SDK or Arm compiler, but the TinyUSB submodule must be initialized. Run them with the
default C compiler or select one before the first configure:

```sh
make test-sanitizers
CC=clang make test-sanitizers
```

The compiler selection is cached in `firmware/build-host-sanitizers`; remove
that generated directory before switching compilers. The tests compile the
same USB audio sample decoder used by the firmware and cover the supported
16-, 20-, and 24-bit subslots, including IEC 61937 words, unaligned byte input,
buffer bounds, and the maximum audio-task batch. They also compile the real USB
descriptors and control callbacks for both audio classes and debug layouts,
checking sample-rate negotiation, sink changes, fallback capabilities, and
unsupported format/rate requests.

The RP2040 firmware remains on its normal embedded build path and is not
instrumented. Host tests cannot validate DMA and PIO timing, multicore races,
interrupt behavior, or flash-blackout behavior on the device.

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

## Inspect silent audio

For a connected adapter that stops producing sound while volume controls still
work, an optional release build exposes read-only USB audio diagnostics while
keeping the same audio interfaces and descriptors:

```sh
cmake -S firmware -B firmware/build-diagnostics-uac2 -G Ninja \
  -DPICOARC_VARIANT=release -DPICOARC_USB_AUDIO_CLASS=uac2 \
  -DPICOARC_USB_DIAGNOSTICS=ON
cmake --build firmware/build-diagnostics-uac2
```

Flash `firmware/build-diagnostics-uac2/picoarc-release-uac2-diagnostics.uf2`
using BOOTSEL, then capture during playback or a failure before reconnecting USB:

```sh
python3 tools/audio_probe.py --seconds 30 > audio-diagnostics.jsonl
```

The probe requires libusb and reports audio counters, ARC gating, DMA progress,
output hardware state, and reset cause without clearing counters or claiming
interfaces.
During a bounded capture it waits for the adapter to return after a KVM switch;
an uptime reset means the board rebooted, while `watchdog_timeout` and
`watchdog_forced` distinguish watchdog resets from hardware resets.
Reset-cause flags require diagnostic protocol version 2; output registers,
nonzero encoded-frame counts, and encoder/buffer hashes require version 3,
and unavailable fields are omitted for older firmware.
Growing USB/frame counters with a stopped DMA block counter indicate a stuck
output core; a closed format gate points to ARC/capability state, while growing
DMA blocks with admitted nonzero audio require checking the carrier and receiver.
Diagnostics are disabled by default and provide evidence for debugging rather
than changing recovery behavior.

Protocol 3 diagnostics also provide an explicit software BOOTSEL command;
ordinary captures remain read-only, and rebooting requires the board's USB
serial number:

```sh
python3 tools/audio_probe.py --bootsel --serial YOUR_BOARD_SERIAL
```

Protocol 4 can also copy the encoded 192-frame buffer currently being consumed
by DMA, without pausing playback:

```sh
python3 tools/audio_probe.py --spdif-block --serial YOUR_BOARD_SERIAL
```

The snapshot includes the encoded words, current PIO instruction and pin-pad
configuration. The host decoder checks preambles, BMC transitions, parity and
sample validity, and reports PCM peak/RMS levels and channel-status bytes.
Copies that cross a DMA buffer handoff are marked unstable and must be retried.
This checks the data supplied to PIO; it does not measure the HDMI waveform or
confirm that the soundbar has locked onto it. Ordinary captures omit this larger
snapshot.

Protocol 5 adds two explicit recovery commands, both requiring the matching
USB serial number:

```sh
python3 tools/audio_probe.py --recover output --serial YOUR_BOARD_SERIAL
python3 tools/audio_probe.py --recover usb --serial YOUR_BOARD_SERIAL
```

Capture the failed stream first, then try one recovery at a time. `output`
asks core 1 to restart its S/PDIF state machine and DMA without restarting USB
or ARC negotiation. `usb` reconnects the USB audio device without rebooting the
adapter or reinitializing the S/PDIF state machine. Both briefly interrupt
playback. Read-only captures report output request/completion counters and USB
reconnect requests; an accepted command is not proof that sound has returned.
These commands are diagnostic experiments, not automatic recovery policies.

Protocol 6 adds read-only HPD/DDC controller state and cached soundbar volume
and mute reports, plus an explicit HDMI recovery command:

```sh
python3 tools/audio_probe.py --recover hdmi --serial YOUR_BOARD_SERIAL
```

This reconnects USB, resets the EDID responder and ARC session, and pulses HPD
while retaining processor uptime and the S/PDIF transmitter configuration.
It briefly interrupts playback and reports the request in subsequent captures.
Test it during a captured silent failure to determine whether that boundary
restores sound; an accepted request alone does not establish recovery.

Protocol 8 retains the last 128 observed USB/ARC state changes, streaming-alt
requests, parsed CEC frames and CEC transmit results in memory, with timestamps
and sequence numbers. Capture this history before a reset; reads preserve it,
while a processor restart clears it:

```sh
python3 tools/audio_probe.py --trace --spdif-block --serial YOUR_BOARD_SERIAL
```

The following manual tests separate parts of the combined HDMI recovery:

| Command | Direct action |
| --- | --- |
| `--recover ddc` | Reset only the EDID I2C controller, FIFOs and byte parser |
| `--recover cec-rx` | Reset only the CEC receiver and parser |
| `--recover cec` | Resend the existing system-audio and ARC-initiation requests |
| `--recover hpd` | Pulse HPD low for 200 ms, retaining cached ARC and EDID state |

Each command requires `--serial`; none explicitly reconnects USB or restarts
the audio transmitter, although soundbar replies can trigger normal ARC/USB
state changes. Try one command at a time during a captured silent failure and
check audibility and the trace before proceeding; these diagnostics introduce
no automatic recovery or idle refresh.
