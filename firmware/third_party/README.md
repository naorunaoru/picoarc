# Third-party sources

## Pico SDK

The Raspberry Pi Pico SDK is a git submodule at `pico-sdk`, pinned to
[`raspberrypi/pico-sdk`](https://github.com/raspberrypi/pico-sdk) commit
[`a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`](https://github.com/raspberrypi/pico-sdk/commit/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779),
the upstream 2.2.0 release. Its own pinned dependencies are initialized
recursively.

## TinyUSB

TinyUSB is a git submodule at `tinyusb`, pinned to
[`hathach/tinyusb`](https://github.com/hathach/tinyusb) commit
[`dae3f9a366bfcddbf9dcf1b48d7500286a849539`](https://github.com/hathach/tinyusb/commit/dae3f9a366bfcddbf9dcf1b48d7500286a849539),
the upstream 0.21.0 release. Its feedback implementation emits UAC1 three-byte
10.14 and UAC2 four-byte 16.16 values. PicoARC uses it only for the default
UAC1 target: the optional full-speed UAC2 target uses an adaptive OUT endpoint
and has no feedback endpoint, avoiding the Windows/macOS feedback-format split.

Initialize the dependencies from the repository root:

```sh
git submodule update --init --recursive
```
