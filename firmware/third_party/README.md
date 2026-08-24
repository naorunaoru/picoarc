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
the upstream 0.21.0 release. It emits the UAC1 three-byte 10.14 and UAC2
four-byte 16.16 asynchronous feedback formats. PicoARC therefore no longer
carries its former full-speed UAC2 feedback patch; the optional UAC2 target is
not feedback-compatible with that legacy macOS-oriented build.

Initialize the dependencies from the repository root:

```sh
git submodule update --init --recursive
```
