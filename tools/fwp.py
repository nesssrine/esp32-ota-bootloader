"""Host-side implementation of the firmware transfer protocol.

Mirrors firmware/components/fw_protocol (same frame format and CRCs).
No dependency on pyserial, so it can be unit-tested anywhere.
"""
from __future__ import annotations

import struct
import zlib
from enum import IntEnum
from typing import List, Optional, Tuple

SOF = 0xA5
RSP_FLAG = 0x80
RSP_NACK = 0xFF
MAX_PAYLOAD = 1024
DATA_HDR_SIZE = 4
MAX_DATA = MAX_PAYLOAD - DATA_HDR_SIZE


class Cmd(IntEnum):
    PING = 0x01
    START = 0x02
    DATA = 0x03
    END = 0x04
    REBOOT = 0x05
    INFO = 0x06


class Status(IntEnum):
    OK = 0x00
    ERR_CRC = 0x01
    ERR_STATE = 0x02
    ERR_LEN = 0x03
    ERR_OFFSET = 0x04
    ERR_FLASH = 0x05
    ERR_IMAGE = 0x06
    ERR_UNKNOWN_CMD = 0x07
    ERR_REJECTED = 0x08


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    """zlib CRC-32, identical to fwp_crc32_update(0, ...) on the device."""
    return zlib.crc32(data) & 0xFFFFFFFF


def encode(cmd: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large ({len(payload)} > {MAX_PAYLOAD})")
    body = struct.pack("<BH", cmd, len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<H", crc16_ccitt(body))


def start_payload(image: bytes) -> bytes:
    return struct.pack("<II", len(image), crc32(image))


def data_payload(offset: int, chunk: bytes) -> bytes:
    return struct.pack("<I", offset) + chunk


class Parser:
    """Incremental frame parser. Skips noise (e.g. log text) until a SOF and
    drops frames whose CRC is wrong."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.crc_errors = 0

    def feed(self, data: bytes) -> List[Tuple[int, bytes]]:
        self._buf.extend(data)
        frames: List[Tuple[int, bytes]] = []
        while True:
            frame = self._next_frame()
            if frame is None:
                return frames
            frames.append(frame)

    def _next_frame(self) -> Optional[Tuple[int, bytes]]:
        while True:
            start = self._buf.find(bytes([SOF]))
            if start < 0:
                self._buf.clear()
                return None
            del self._buf[:start]
            if len(self._buf) < 4:
                return None
            cmd, length = struct.unpack_from("<BH", self._buf, 1)
            if length > MAX_PAYLOAD:
                del self._buf[0]  # false SOF, resync on the next one
                continue
            total = 4 + length + 2
            if len(self._buf) < total:
                return None
            body = bytes(self._buf[1 : 4 + length])
            (crc_rx,) = struct.unpack_from("<H", self._buf, 4 + length)
            if crc_rx != crc16_ccitt(body):
                self.crc_errors += 1
                del self._buf[0]
                continue
            del self._buf[:total]
            return cmd, body[3:]
