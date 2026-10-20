#!/usr/bin/env python3
"""Read PicoARC audio state; recovery and BOOTSEL require explicit actions."""

import argparse
import ctypes
import ctypes.util
import datetime
import json
import math
import struct
import sys
import time


FIELDS = (
    "version", "uptime_ms", "flags", "sample_rate", "active_alt",
    "rx_packets", "rx_bytes", "nonzero_bytes", "written_frames",
    "gated_frames", "buffered_frames", "spdif_mode", "dma_blocks",
    "dma_remaining", "dma_ctrl", "pio_fdebug",
)
FLAGS = (
    "streaming", "output_enabled", "audio_gate_open", "usb_mounted",
    "usb_suspended", "arc_initiated", "system_audio_enabled",
    "arc_caps_ready", "hdmi_connected",
)
RESET_FLAGS = ("watchdog_timeout", "watchdog_forced")
BOOTSEL_FLAGS = ("software_bootsel",)
RECOVERY_FLAGS = ("targeted_recovery",)
RECOVERY_FIELDS = (
    "version", "output_requests", "output_completions", "last_output_restart_ms",
    "output_reserved", "usb_reconnect_requests", "last_usb_reconnect_ms",
) + tuple(f"reserved_{i}" for i in range(9))
RECOVERY_V2_FIELDS = RECOVERY_FIELDS[:7] + (
    "hdmi_recovery_requests", "last_hdmi_recovery_ms",
) + tuple(f"reserved_{i}" for i in range(7))
RECOVERY_V4_FIELDS = RECOVERY_V2_FIELDS[:9] + (
    "isolated_requests", "last_isolated_ms", "last_isolated_action",
) + tuple(f"reserved_{i}" for i in range(4))
RECOVERY_ACTIONS = {"output": 1, "usb": 2, "hdmi": 3, "ddc": 4,
                    "hpd": 5, "cec": 6, "cec-rx": 7}
TRACE_SIZE = 32 + 128 * 32
HDMI_FIELDS = (
    "version", "hpd_gpio_ctrl", "hpd_gpio_status", "gpio_levels",
    "cec_audio_status", "ddc_enable", "ddc_status", "ddc_raw_intr_stat",
    "ddc_tx_abrt_source", "ddc_rx_fifo_level", "ddc_tx_fifo_level",
    "ddc_offset_writes", "ddc_read_requests", "ddc_bytes_sent",
    "ddc_edid_offset", "ddc_write_index",
)
OUTPUT_FIELDS = (
    "version", "gpio_ctrl", "gpio_status", "pio_ctrl", "pio_clkdiv",
    "pio_execctrl", "pio_shiftctrl", "pio_pinctrl", "pio_addr", "pio_fstat",
    "dma_read_addr", "stream_format", "encoded_nonzero_frames",
    "encoder_table_hash", "dma_block_hash", "silence_block_hash",
)
BLOCK_FIELDS = (
    "version", "stable", "dma_blocks_before", "dma_blocks_after",
    "block_index", "dma_remaining_before", "dma_remaining_after",
    "spdif_mode", "stream_format", "pio_clkdiv", "pio_instruction",
    "gpio_pad", "clk_sys_hz", "uptime_ms", "reserved", "word_count",
)
COUNTERS = (
    "rx_packets", "rx_bytes", "nonzero_bytes", "written_frames",
    "gated_frames", "dma_blocks",
)


def load_usb():
    candidates = (
        ctypes.util.find_library("usb-1.0"),
        "/opt/homebrew/lib/libusb-1.0.dylib",
        "/usr/local/lib/libusb-1.0.dylib",
    )
    for path in candidates:
        if not path:
            continue
        try:
            usb = ctypes.CDLL(path)
            break
        except OSError:
            continue
    else:
        raise RuntimeError("libusb-1.0 is required (macOS: brew install libusb)")

    usb.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    usb.libusb_init.restype = ctypes.c_int
    usb.libusb_exit.argtypes = [ctypes.c_void_p]
    usb.libusb_exit.restype = None
    usb.libusb_open_device_with_vid_pid.argtypes = [
        ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16,
    ]
    usb.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    usb.libusb_close.argtypes = [ctypes.c_void_p]
    usb.libusb_close.restype = None
    usb.libusb_error_name.argtypes = [ctypes.c_int]
    usb.libusb_error_name.restype = ctypes.c_char_p
    usb.libusb_control_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_uint16, ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint16, ctypes.c_uint,
    ]
    usb.libusb_control_transfer.restype = ctypes.c_int
    usb.libusb_get_string_descriptor_ascii.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int,
    ]
    usb.libusb_get_string_descriptor_ascii.restype = ctypes.c_int
    return usb


def read_control(usb, handle, request_type, request, value, index, length):
    data = (ctypes.c_ubyte * length)()
    result = usb.libusb_control_transfer(
        handle, request_type, request, value, index, data, length, 1000,
    )
    if result < 0:
        return None, usb.libusb_error_name(result).decode("ascii")
    if result != length:
        return None, f"short response: {result}/{length} bytes"
    return bytes(data), None


def decode_spdif_block(words):
    """Decode a 192-frame DMA block, independently checking BMC and framing."""
    if len(words) != 768:
        raise ValueError("expected 768 encoded words")
    errors = {"preamble": 0, "bmc": 0, "parity": 0, "validity": 0}
    samples = [[], []]
    channel_status = [bytearray(24), bytearray(24)]
    previous_level = None
    # The PIO shifts each word LSB first. These are the on-wire preambles
    # with their first half-bit in bit 0, before polarity inversion.
    for subframe in range(384):
        frame, channel = divmod(subframe, 2)
        raw = words[subframe * 2] | (words[subframe * 2 + 1] << 32)
        expected = 0x27 if channel else (0x17 if frame == 0 else 0x47)
        preamble = raw & 0xff
        if preamble not in (expected, expected ^ 0xff):
            errors["preamble"] += 1
        elif previous_level is not None and preamble != (expected ^ (0xff if previous_level else 0)):
            errors["preamble"] += 1
        level = (raw >> 7) & 1
        payload = 0
        for bit in range(28):
            first = (raw >> (8 + bit * 2)) & 1
            second = (raw >> (9 + bit * 2)) & 1
            if first == level:
                errors["bmc"] += 1
            payload |= (first ^ second) << bit
            level = second
        previous_level = level
        errors["parity"] += payload.bit_count() & 1
        errors["validity"] += (payload >> 24) & 1
        sample = payload & 0xffffff
        if sample & 0x800000:
            sample -= 1 << 24
        samples[channel].append(sample)
        channel_status[channel][frame // 8] |= ((payload >> 26) & 1) << (frame % 8)
    return {
        "errors": errors,
        "peak_fraction": [max(abs(x) for x in s) / (1 << 23) for s in samples],
        "rms_fraction": [math.sqrt(sum(x * x for x in s) / len(s)) / (1 << 23) for s in samples],
        "nonzero_frames": sum(l != 0 or r != 0 for l, r in zip(*samples)),
        "channel_status_hex": [bytes(s).hex() for s in channel_status],
    }


def decode_trace(data):
    if len(data) != TRACE_SIZE:
        raise ValueError("wrong trace snapshot size")
    version, uptime, total, count, capacity, entry_size, _, _ = struct.unpack("<8I", data[:32])
    if version != 1 or capacity != 128 or entry_size != 32 or count > capacity:
        raise ValueError("unknown or invalid trace layout")
    events = []
    names = {1: "state", 2: "cec_tx", 3: "cec_rx", 4: "recovery", 5: "usb_alt"}
    for i in range(count):
        sequence, ms, kind, flags, value, payload = struct.unpack_from("<IIHHI16s", data, 32 + i * 32)
        event = dict(sequence=sequence, uptime_ms=ms, kind=names.get(kind, kind),
                     flags=flags, value=value)
        if kind == 1:
            event.update(sample_rate=value, active_alt=payload[0], spdif_mode=payload[1],
                         state={name: bool(flags & (1 << bit)) for bit, name in enumerate(FLAGS)})
        elif kind in (2, 3):
            if not 1 <= value <= 16:
                raise ValueError("invalid CEC frame length in trace")
            event["frame_hex"] = payload[:value].hex()
            if kind == 2:
                event["result"] = "bus_busy" if flags & 2 else ("ok" if flags & 1 else "ack_failed")
        elif kind == 4:
            event["action"] = next((name for name, code in RECOVERY_ACTIONS.items() if code == value), value)
            event["phase"] = {0: "requested", 1: "started", 2: "completed"}.get(flags, flags)
        events.append(event)
    return dict(version=version, uptime_ms=uptime, total_entries=total,
                count=count, capacity=capacity, events=events)


def recovery_value(result, action):
    if not result.get("state", {}).get("targeted_recovery"):
        raise RuntimeError("installed firmware does not advertise targeted recovery support")
    minimum_version = 8 if RECOVERY_ACTIONS[action] >= 4 else (6 if action == "hdmi" else 5)
    if result["diagnostics"]["version"] < minimum_version:
        raise RuntimeError(f"{action} recovery requires diagnostic protocol {minimum_version}")
    return RECOVERY_ACTIONS[action]


def capture(usb, handle, pid, include_block=False, include_trace=False):
    debug = pid in (0x4013, 0x4017)
    uac2 = pid in (0x4016, 0x4017)
    control_itf = 2 if debug else 0
    data, error = read_control(usb, handle, 0x81, 0x0A, 0, control_itf + 1, 1)
    result = {
        "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "usb_alt": error if error else data[0],
    }
    if uac2:
        data, error = read_control(usb, handle, 0xA1, 1, 0x0100,
                                   0x0400 | control_itf, 4)
    else:
        data, error = read_control(usb, handle, 0xA2, 0x81, 0x0100, 0x03, 3)
    result["usb_sample_rate"] = error if error else int.from_bytes(data, "little")
    data, error = read_control(usb, handle, 0xC0, 0x5A, 0, 0, 64)
    if error:
        result["diagnostics_error"] = error
        return result
    diagnostics = dict(zip(FIELDS, struct.unpack("<16I", data)))
    if diagnostics["version"] not in (1, 2, 3, 4, 5, 6, 8):
        result["diagnostics_error"] = f"unknown version: {diagnostics['version']}"
        return result
    result["diagnostics"] = diagnostics
    flags = FLAGS + (RESET_FLAGS if diagnostics["version"] >= 2 else ())
    if diagnostics["version"] >= 3:
        flags += BOOTSEL_FLAGS
    if diagnostics["version"] >= 5:
        flags += RECOVERY_FLAGS
    result["state"] = {
        name: bool(diagnostics["flags"] & (1 << i))
        for i, name in enumerate(flags)
    }
    if diagnostics["version"] >= 3:
        data, error = read_control(usb, handle, 0xC0, 0x5B, 0, 0, 64)
        if error:
            result["output_hardware_error"] = error
        else:
            output = dict(zip(OUTPUT_FIELDS, struct.unpack("<16I", data)))
            if output["version"] != 1:
                result["output_hardware_error"] = f"unknown version: {output['version']}"
            else:
                result["output_hardware"] = output
    if diagnostics["version"] >= 5:
        data, error = read_control(usb, handle, 0xC0, 0x5F, 0, 0, 64)
        if error:
            result["recovery_error"] = error
        else:
            values = struct.unpack("<16I", data)
            fields = {1: RECOVERY_FIELDS, 2: RECOVERY_V2_FIELDS, 4: RECOVERY_V4_FIELDS}.get(values[0], RECOVERY_FIELDS)
            recovery = dict(zip(fields, values))
            if recovery["version"] not in (1, 2, 4):
                result["recovery_error"] = f"unknown recovery version: {recovery['version']}"
            else:
                result["recovery"] = recovery
    if diagnostics["version"] >= 6:
        data, error = read_control(usb, handle, 0xC0, 0x60, 0, 0, 64)
        if error:
            result["hdmi_hardware_error"] = error
        else:
            hdmi = dict(zip(HDMI_FIELDS, struct.unpack("<16I", data)))
            if hdmi["version"] != 1:
                result["hdmi_hardware_error"] = f"unknown HDMI version: {hdmi['version']}"
            else:
                status = hdmi["cec_audio_status"]
                hdmi["cec_reported_audio"] = {
                    "volume_known": bool(status & 0x100),
                    "mute_known": bool(status & 0x200),
                    "volume": status & 0x7f,
                    "muted": bool(status & 0x80),
                }
                result["hdmi_hardware"] = hdmi
    if include_trace:
        if diagnostics["version"] < 8:
            result["trace_error"] = "retained event traces require protocol 8"
        else:
            data, error = read_control(usb, handle, 0xC0, 0x61, 0, 0, TRACE_SIZE)
            if error:
                result["trace_error"] = error
            else:
                try:
                    result["trace"] = decode_trace(data)
                except ValueError as exc:
                    result["trace_error"] = str(exc)
    if include_block:
        if diagnostics["version"] < 4:
            result["spdif_block_error"] = "encoded-block snapshots require protocol 4"
        else:
            data, error = read_control(usb, handle, 0xC0, 0x5D, 0, 0, 3136)
            if error:
                result["spdif_block_error"] = error
            else:
                block = dict(zip(BLOCK_FIELDS, struct.unpack("<16I", data[:64])))
                result["spdif_block"] = block
                if block["version"] != 1 or block["word_count"] != 768:
                    result["spdif_block_error"] = "unknown encoded-block layout"
                elif not block["stable"]:
                    result["spdif_block_error"] = "DMA changed during the snapshot; retry"
                else:
                    block["words_le_hex"] = data[64:].hex()
                    if block["spdif_mode"] != 0:
                        block["decoded"] = decode_spdif_block(struct.unpack("<768I", data[64:]))
    return result


def device_serial(usb, handle):
    descriptor, error = read_control(usb, handle, 0x80, 6, 0x0100, 0, 18)
    if error:
        raise RuntimeError(f"cannot read device descriptor: {error}")
    data = (ctypes.c_ubyte * 256)()
    count = usb.libusb_get_string_descriptor_ascii(handle, descriptor[16], data, 256)
    if count < 0:
        raise RuntimeError(f"cannot read device serial: {usb.libusb_error_name(count).decode('ascii')}")
    return bytes(data[:count]).decode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=lambda value: int(value, 0), default=0x4016,
                        choices=(0x4012, 0x4013, 0x4016, 0x4017))
    parser.add_argument("--seconds", type=float, default=0,
                        help="capture for this duration; default is one snapshot")
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--serial", help="require this USB serial number")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--bootsel", action="store_true",
                         help="reboot to BOOTSEL (requires --serial and protocol 3)")
    actions.add_argument("--recover", choices=tuple(RECOVERY_ACTIONS),
                         help="manual output/USB/HDMI recovery, or isolated DDC/HPD/CEC tests (protocol 8); requires --serial")
    parser.add_argument("--spdif-block", action="store_true",
                        help="include and decode a stable encoded DMA block (protocol 4)")
    parser.add_argument("--trace", action="store_true",
                        help="include retained state and CEC history without clearing it (protocol 8)")
    args = parser.parse_args()
    if args.seconds < 0 or args.interval <= 0:
        parser.error("seconds must be nonnegative and interval must be positive")
    if (args.bootsel or args.recover) and (not args.serial or args.seconds != 0 or args.spdif_block or args.trace):
        parser.error("actions require --serial and cannot be combined with capture options")

    usb = load_usb()
    context = ctypes.c_void_p()
    status = usb.libusb_init(ctypes.byref(context))
    if status:
        raise RuntimeError(usb.libusb_error_name(status).decode("ascii"))
    handle = None
    try:
        deadline = time.monotonic() + args.seconds
        previous = None
        previous_output = None
        while True:
            if not handle:
                handle = usb.libusb_open_device_with_vid_pid(context, 0xCAFE, args.pid)
                if handle and args.serial:
                    actual_serial = device_serial(usb, handle)
                    if actual_serial != args.serial:
                        raise RuntimeError(f"device serial mismatch: expected {args.serial}, got {actual_serial}")
            if handle:
                result = capture(usb, handle, args.pid, args.spdif_block, args.trace)
                if args.bootsel:
                    if not result.get("state", {}).get("software_bootsel"):
                        raise RuntimeError("installed firmware does not advertise software BOOTSEL support")
                    print(json.dumps(result), flush=True)
                    status = usb.libusb_control_transfer(handle, 0x40, 0x5C, 0, 0, None, 0, 1000)
                    # The board may disappear immediately after acknowledging
                    # the status stage. Verify BOOTSEL separately with picotool.
                    if status not in (0, -4):
                        raise RuntimeError(usb.libusb_error_name(status).decode("ascii"))
                    print(json.dumps({"action": "bootsel_requested", "serial": args.serial}), flush=True)
                    break
                if args.recover:
                    value = recovery_value(result, args.recover)
                    print(json.dumps(result), flush=True)
                    status = usb.libusb_control_transfer(handle, 0x40, 0x5E, value, 0, None, 0, 1000)
                    if status != 0 and not (args.recover in ("usb", "hdmi") and status == -4):
                        raise RuntimeError(usb.libusb_error_name(status).decode("ascii"))
                    print(json.dumps({"action": f"{args.recover}_recovery_requested", "serial": args.serial}), flush=True)
                    break
                if "LIBUSB_ERROR_NO_DEVICE" in result.values():
                    usb.libusb_close(handle)
                    handle = None
            else:
                if args.seconds == 0:
                    raise RuntimeError(f"PicoARC cafe:{args.pid:04x} is not accessible")
                result = {
                    "time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "device_error": "PicoARC is not accessible; waiting for reconnection",
                }
            current = result.get("diagnostics")
            current_output = result.get("output_hardware")
            if current and previous and current["uptime_ms"] > previous["uptime_ms"]:
                elapsed = (current["uptime_ms"] - previous["uptime_ms"]) / 1000
                result["per_second"] = {
                    name: round(((current[name] - previous[name]) & 0xFFFFFFFF) / elapsed)
                    for name in COUNTERS
                }
                if current_output and previous_output:
                    delta = (current_output["encoded_nonzero_frames"] -
                             previous_output["encoded_nonzero_frames"]) & 0xFFFFFFFF
                    result["per_second"]["encoded_nonzero_frames"] = round(delta / elapsed)
            print(json.dumps(result), flush=True)
            previous = current
            previous_output = current_output
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))
    finally:
        if handle:
            usb.libusb_close(handle)
        usb.libusb_exit(context)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, RuntimeError) as exc:
        sys.exit(str(exc))
