# Third-party sources

## Pico SDK

The Raspberry Pi Pico SDK is a git submodule at `pico-sdk`, pinned to
[`raspberrypi/pico-sdk`](https://github.com/raspberrypi/pico-sdk) commit
[`a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`](https://github.com/raspberrypi/pico-sdk/commit/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779),
the upstream 2.2.0 release. Its own pinned dependencies are initialized
recursively.

## TinyUSB

TinyUSB is a git submodule at `tinyusb`, pinned to
[`naorunaoru/tinyusb`](https://github.com/naorunaoru/tinyusb) commit
[`3bdd2e5edc198e06e91263c2afc877b7cbb9a3da`](https://github.com/naorunaoru/tinyusb/commit/3bdd2e5edc198e06e91263c2afc877b7cbb9a3da).
It is based on upstream 0.21.0 and changes asynchronous feedback encoding to
follow the USB bus speed: full-speed uses the three-byte 10.14 format and
high-speed uses four-byte 16.16.

This is required because PicoARC presents UAC2 on the RP2040's full-speed USB
controller. TinyUSB 0.20.0 and later otherwise select the four-byte format from
the UAC version, which macOS does not accept for this endpoint.

Initialize the dependencies from the repository root:

```sh
git submodule update --init --recursive
```
