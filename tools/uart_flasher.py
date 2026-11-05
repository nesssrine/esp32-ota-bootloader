#!/usr/bin/env python3
"""Send a firmware image to the ESP32 UART updater.

Examples:
    python tools/uart_flasher.py COM5 --info
    python tools/uart_flasher.py COM5 firmware/build/esp32_ota_bootloader.bin
    python tools/uart_flasher.py /dev/ttyUSB0 app.bin --baud 115200 --no-reboot
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: pip install -r tools/requirements.txt")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fwp  # noqa: E402


class UpdateError(RuntimeError):
    pass


class Device:
    def __init__(self, port: str, baud: int, retries: int, verbose: bool) -> None:
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        # On ESP32 DevKits, DTR/RTS drive EN and GPIO0: keep them released so
        # opening the port does not reset the board or enter download mode.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.parser = fwp.Parser()
        self.retries = retries
        self.verbose = verbose

    def close(self) -> None:
        self.ser.close()

    def _wait_response(self, cmd: int, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.ser.read(self.ser.in_waiting or 1)
            if not data:
                continue
            for rsp_cmd, payload in self.parser.feed(data):
                if rsp_cmd in (cmd | fwp.RSP_FLAG, fwp.RSP_NACK) and payload:
                    return rsp_cmd, fwp.Status(payload[0]), payload[1:]
        return None

    def request(self, cmd: fwp.Cmd, payload: bytes = b"", timeout: float = 1.0) -> bytes:
        frame = fwp.encode(cmd, payload)
        last_error = "no response"
        for attempt in range(1, self.retries + 1):
            self.ser.reset_input_buffer()
            self.ser.write(frame)
            rsp = self._wait_response(cmd, timeout)
            if rsp is None:
                last_error = "timeout"
            else:
                rsp_cmd, status, extra = rsp
                if status == fwp.Status.OK and rsp_cmd != fwp.RSP_NACK:
                    return extra
                last_error = status.name
                # Errors that a retransmission cannot fix
                if rsp_cmd != fwp.RSP_NACK and status not in (fwp.Status.ERR_CRC,):
                    raise UpdateError(f"{cmd.name} refused by device: {status.name}")
            if self.verbose:
                print(f"\n  {cmd.name} attempt {attempt} failed ({last_error}), retrying")
        raise UpdateError(f"{cmd.name} failed after {self.retries} attempts: {last_error}")


def progress(done: int, total: int, t0: float) -> None:
    pct = 100 * done / total
    bar = "#" * int(pct // 2.5)
    speed = done / max(time.monotonic() - t0, 1e-3) / 1024
    print(f"\r  [{bar:<40}] {pct:5.1f}%  {done}/{total} B  {speed:5.1f} KiB/s", end="", flush=True)


def flash(dev: Device, image: bytes, reboot: bool) -> None:
    print(f"Image: {len(image)} bytes, CRC32 0x{fwp.crc32(image):08X}")

    dev.request(fwp.Cmd.START, fwp.start_payload(image), timeout=3.0)

    t0 = time.monotonic()
    for offset in range(0, len(image), fwp.MAX_DATA):
        chunk = image[offset : offset + fwp.MAX_DATA]
        dev.request(fwp.Cmd.DATA, fwp.data_payload(offset, chunk), timeout=2.0)
        progress(offset + len(chunk), len(image), t0)
    print()

    print("Verifying image on device (CRC32 + SHA-256)...")
    dev.request(fwp.Cmd.END, timeout=10.0)
    print("Update written and selected for next boot.")

    if reboot:
        dev.request(fwp.Cmd.REBOOT)
        print("Device is rebooting into the new firmware.")


def main() -> int:
    ap = argparse.ArgumentParser(description="ESP32 UART firmware updater")
    ap.add_argument("port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("firmware", nargs="?", type=Path, help="application .bin to send")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--retries", type=int, default=5)
    ap.add_argument("--info", action="store_true", help="only print running firmware info")
    ap.add_argument("--no-reboot", action="store_true", help="do not reboot after update")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not args.info and args.firmware is None:
        ap.error("firmware file required (or use --info)")

    dev = Device(args.port, args.baud, args.retries, args.verbose)
    try:
        dev.request(fwp.Cmd.PING)
        info = dev.request(fwp.Cmd.INFO).decode(errors="replace")
        print(f"Device: {info}")
        if not args.info:
            flash(dev, args.firmware.read_bytes(), reboot=not args.no_reboot)
    except UpdateError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
