"""Unit tests for the host protocol module (python -m unittest discover -s tools/tests)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fwp  # noqa: E402

# Same bytes as REF_PING / REF_START in tests/host/test_fw_protocol.c
REF_PING = bytes([0xA5, 0x01, 0x00, 0x00, 0xAC, 0xFB])
REF_START = bytes([0xA5, 0x02, 0x08, 0x00, 0x00, 0x10, 0x00, 0x00,
                   0x26, 0x39, 0xF4, 0xCB, 0x4D, 0xF5])


class CrcTests(unittest.TestCase):
    def test_reference_values(self):
        self.assertEqual(fwp.crc16_ccitt(b"123456789"), 0x29B1)
        self.assertEqual(fwp.crc32(b"123456789"), 0xCBF43926)


class EncodeTests(unittest.TestCase):
    def test_matches_c_reference_frames(self):
        self.assertEqual(fwp.encode(fwp.Cmd.PING), REF_PING)
        start = fwp.encode(fwp.Cmd.START, (0x1000).to_bytes(4, "little")
                           + (0xCBF43926).to_bytes(4, "little"))
        self.assertEqual(start, REF_START)

    def test_payload_too_large(self):
        with self.assertRaises(ValueError):
            fwp.encode(fwp.Cmd.DATA, bytes(fwp.MAX_PAYLOAD + 1))


class ParserTests(unittest.TestCase):
    def test_roundtrip_max_payload(self):
        payload = bytes((i * 7) & 0xFF for i in range(fwp.MAX_PAYLOAD))
        frames = fwp.Parser().feed(fwp.encode(fwp.Cmd.DATA, payload))
        self.assertEqual(frames, [(fwp.Cmd.DATA, payload)])

    def test_byte_by_byte(self):
        parser = fwp.Parser()
        out = []
        for b in REF_START:
            out += parser.feed(bytes([b]))
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0][0], fwp.Cmd.START)

    def test_skips_log_noise(self):
        stream = b"I (1234) main: hello\r\n" + REF_PING + b"more logs" + REF_START
        frames = fwp.Parser().feed(stream)
        self.assertEqual([cmd for cmd, _ in frames], [fwp.Cmd.PING, fwp.Cmd.START])

    def test_drops_corrupted_frame(self):
        bad = bytearray(REF_START)
        bad[6] ^= 0x01
        parser = fwp.Parser()
        self.assertEqual(parser.feed(bytes(bad)), [])
        self.assertGreaterEqual(parser.crc_errors, 1)
        self.assertEqual(len(parser.feed(REF_PING)), 1)


if __name__ == "__main__":
    unittest.main()
