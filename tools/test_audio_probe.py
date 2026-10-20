import struct
import unittest
from unittest.mock import patch

from audio_probe import capture, decode_spdif_block, decode_trace, recovery_value, TRACE_SIZE


def silent_block():
    words = []
    for frame in range(192):
        words.extend((0x33333317 if frame == 0 else 0x33333347,
                      0x33333333, 0x33333327, 0x33333333))
    return words


def replace_payload(words, subframe, payload):
    """Construct BMC from logical bits using the transition rule."""
    offset = subframe * 2
    encoded = words[offset] & 0xff
    level = (encoded >> 7) & 1
    for bit in range(28):
        level ^= 1
        encoded |= level << (8 + bit * 2)
        level ^= (payload >> bit) & 1
        encoded |= level << (9 + bit * 2)
    words[offset] = encoded & 0xffffffff
    words[offset + 1] = encoded >> 32


class SpdifDecoderTests(unittest.TestCase):
    def test_golden_silence(self):
        result = decode_spdif_block(silent_block())
        self.assertEqual(result['errors'], dict(preamble=0, bmc=0, parity=0, validity=0))
        self.assertEqual(result['peak_fraction'], [0, 0])
        self.assertEqual(result['nonzero_frames'], 0)

    def test_signed_pcm_and_channel_status(self):
        words = silent_block()
        for subframe in range(384):
            payload = 0x800000 if subframe % 2 == 0 else 0x7fffff
            if subframe in (2, 3):
                payload |= 1 << 26  # Non-PCM channel-status bit at frame 1.
            payload |= (payload.bit_count() & 1) << 27
            replace_payload(words, subframe, payload)
        result = decode_spdif_block(words)
        self.assertEqual(result['errors'], dict(preamble=0, bmc=0, parity=0, validity=0))
        self.assertEqual(result['peak_fraction'], [1, 0x7fffff / (1 << 23)])
        self.assertEqual(result['nonzero_frames'], 192)
        self.assertEqual(result['channel_status_hex'], ['02' + '00' * 23] * 2)

    def test_corrupt_parity(self):
        words = silent_block()
        replace_payload(words, 0, 1)
        self.assertEqual(decode_spdif_block(words)['errors']['parity'], 1)

    def test_corrupt_bmc_boundary(self):
        words = silent_block()
        words[0] ^= 3 << 8  # Keep data bit zero but violate its boundary.
        result = decode_spdif_block(words)
        self.assertEqual(result['errors']['bmc'], 2)
        self.assertEqual(result['errors']['parity'], 0)

    def test_corrupt_preamble(self):
        words = silent_block()
        words[0] ^= 1 << 3
        self.assertEqual(decode_spdif_block(words)['errors']['preamble'], 1)

    def test_invalid_sample_flag(self):
        words = silent_block()
        replace_payload(words, 0, (1 << 24) | (1 << 27))
        result = decode_spdif_block(words)
        self.assertEqual(result['errors']['validity'], 1)
        self.assertEqual(result['errors']['parity'], 0)

    def test_wrong_block_size(self):
        with self.assertRaises(ValueError):
            decode_spdif_block([])


class ProtocolCompatibilityTests(unittest.TestCase):
    def capture_version(self, version, recovery_version, include_trace=False):
        requests = []

        def read(_usb, _handle, request_type, request, value, index, length):
            requests.append((request_type, request, value, index, length))
            if request_type == 0x81:
                return b'\x03', None
            if request_type == 0xa1:
                return struct.pack('<I', 96000), None
            if request == 0x5a:
                return struct.pack('<16I', version, 1000, 4096, 96000, *([0] * 12)), None
            if request == 0x5b:
                return struct.pack('<16I', 1, *([0] * 15)), None
            if request == 0x5f:
                return struct.pack('<16I', recovery_version, *range(1, 16)), None
            if request == 0x60:
                values = [1, 5, 12288, 0, 0x300 | 17] + [0] * 11
                return struct.pack('<16I', *values), None
            if request == 0x61:
                return trace_packet(), None
            raise AssertionError(f'unexpected request {request:#x}')

        with patch('audio_probe.read_control', side_effect=read):
            result = capture(None, None, 0x4016, include_trace=include_trace)
        self.assertTrue(all(request[0] & 0x80 for request in requests))
        return result, requests

    def test_v5_retains_original_recovery_layout(self):
        result, requests = self.capture_version(5, 1)
        self.assertEqual(result['recovery']['reserved_0'], 7)
        self.assertNotIn('hdmi_hardware', result)
        self.assertNotIn('hdmi_recovery_requests', result['recovery'])
        self.assertNotIn(0x60, [request[1] for request in requests])

    def test_v6_decodes_hdmi_and_extended_recovery(self):
        result, requests = self.capture_version(6, 2)
        self.assertEqual(result['recovery']['hdmi_recovery_requests'], 7)
        self.assertEqual(result['recovery']['last_hdmi_recovery_ms'], 8)
        self.assertEqual(result['hdmi_hardware']['cec_reported_audio'], {
            'volume_known': True, 'mute_known': True, 'volume': 17, 'muted': False,
        })
        self.assertIn(0x60, [request[1] for request in requests])

    def test_v8_reads_trace_and_isolated_recovery_status(self):
        result, requests = self.capture_version(8, 4, True)
        self.assertEqual(result['recovery']['isolated_requests'], 9)
        self.assertEqual(result['recovery']['last_isolated_action'], 11)
        self.assertEqual(result['trace']['count'], 2)
        self.assertIn((0xc0, 0x61, 0, 0, TRACE_SIZE), requests)
        self.assertEqual(recovery_value(result, 'ddc'), 4)
        self.assertEqual(recovery_value(result, 'hpd'), 5)
        self.assertEqual(recovery_value(result, 'cec'), 6)
        self.assertEqual(recovery_value(result, 'cec-rx'), 7)

    def test_older_firmware_rejects_isolated_commands(self):
        result, requests = self.capture_version(6, 2, True)
        self.assertIn('trace_error', result)
        self.assertNotIn(0x61, [request[1] for request in requests])
        for action in ('ddc', 'hpd', 'cec', 'cec-rx'):
            with self.assertRaises(RuntimeError):
                recovery_value(result, action)


def trace_packet():
    header = struct.pack('<8I', 1, 5000, 15, 2, 128, 32, 0, 0)
    state = struct.pack('<IIHHI16s', 14, 4000, 1, 0x1ef, 96000, b'\x03\x02')
    rx = struct.pack('<IIHHI16s', 15, 4200, 3, 0, 2, b'\x50\xc0')
    return header + state + rx + bytes(TRACE_SIZE - len(header) - len(state) - len(rx))


class TraceDecoderTests(unittest.TestCase):
    def test_retained_state_and_cec_frame(self):
        result = decode_trace(trace_packet())
        self.assertEqual(result['total_entries'], 15)
        self.assertEqual(result['events'][0]['sequence'], 14)
        self.assertTrue(result['events'][0]['state']['audio_gate_open'])
        self.assertEqual(result['events'][0]['sample_rate'], 96000)
        self.assertEqual(result['events'][0]['active_alt'], 3)
        self.assertEqual(result['events'][1]['frame_hex'], '50c0')

    def test_invalid_layout_and_truncated_capture(self):
        packet = bytearray(trace_packet())
        struct.pack_into('<I', packet, 12, 129)
        with self.assertRaises(ValueError):
            decode_trace(packet)
        with self.assertRaises(ValueError):
            decode_trace(trace_packet()[:-1])

    def test_invalid_cec_frame_length(self):
        packet = bytearray(trace_packet())
        struct.pack_into('<I', packet, 32 + 32 + 12, 17)
        with self.assertRaises(ValueError):
            decode_trace(packet)


if __name__ == '__main__':
    unittest.main()
