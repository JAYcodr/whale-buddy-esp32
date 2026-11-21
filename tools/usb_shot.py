#!/usr/bin/env python3
"""Capture an RGB565 screenshot from the ESP32 over USB CDC serial and save as PNG."""

import argparse
import struct
import sys
import time
import serial
from PIL import Image


def capture(port: str, baud: int, out_path: str, timeout: float = 5.0) -> None:
    with serial.Serial(port, baud, timeout=0.5) as ser:
        ser.reset_input_buffer()
        ser.write(b'{"cmd":"shot"}\n')
        ser.flush()

        deadline = time.monotonic() + timeout
        buf = bytearray()
        marker = b"NVSH"
        idx = -1
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                idx = buf.find(marker)
                if idx != -1 and len(buf) >= idx + 12:
                    break
        if idx == -1 or len(buf) < idx + 12:
            raise RuntimeError(f"NVSH header not received (got {len(buf)} bytes)")

        w, h, nbytes = struct.unpack_from("<HHI", buf, idx + 4)
        payload = bytearray(buf[idx + 12:])
        rxDeadline = time.monotonic() + 10.0
        while len(payload) < nbytes and time.monotonic() < rxDeadline:
            chunk = ser.read(min(16384, nbytes - len(payload)))
            if chunk:
                payload.extend(chunk)
        if len(payload) < nbytes:
            raise RuntimeError(f"Incomplete frame: {len(payload)}/{nbytes} bytes")

        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            c = payload[2 * i] | (payload[2 * i + 1] << 8)
            rgb[3 * i + 0] = ((c >> 11) & 0x1F) * 255 // 31
            rgb[3 * i + 1] = ((c >> 5) & 0x3F) * 255 // 63
            rgb[3 * i + 2] = (c & 0x1F) * 255 // 31

        img = Image.frombytes("RGB", (w, h), bytes(rgb))
        img.save(out_path)
        print(f"Saved {out_path} ({w}x{h}, {nbytes} bytes)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem21101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="tools/sim/out/device_shot.png")
    args = ap.parse_args()
    capture(args.port, args.baud, args.out)


if __name__ == "__main__":
    main()
