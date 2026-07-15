# TinyUSB

PicoARC bundles a trimmed TinyUSB 0.21.0 source snapshot from:

https://github.com/hathach/tinyusb/releases/tag/0.21.0

Source archive:
https://github.com/hathach/tinyusb/archive/refs/tags/0.21.0.tar.gz

SHA-256: `b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01`

Only the source tree and RP2040 CMake board support needed by the Pico SDK are
included. The upstream license is preserved in `LICENSE`.

The version is pinned because PicoARC depends on UAC2 isochronous OUT endpoint
recovery across alternate settings with different maximum packet sizes. Update
this snapshot and the UAC2 integration together, then build both firmware
variants and test host format switching before changing the pinned version.

## PicoARC integration patch

PicoARC runs UAC2 over the RP2040 full-speed USB controller. TinyUSB 0.21.0's
audio feedback path selects the full-speed 10.14 wire format by UAC version,
which leaves UAC2 sending high-speed-style 16.16 feedback. PicoARC selects the
feedback format by negotiated bus speed instead and sends the matching
three-byte full-speed packet. This is required for sustained playback on macOS.
