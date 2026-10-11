#!/usr/bin/env python3
"""Grab frames from a C5VRX built with CONFIG_C5VRX_LINK_MODE ('g' key) and
save them as PNG (and PGM).

usage: python tools/link_frame.py [PORT] [--count N] [--out DIR] [--keys KEYS]
       python tools/link_frame.py --file CAPTURE.txt

The firmware answers 'g' with

    [FRAME w=224 h=168 rows=168 std=PAL line_us=64.000 ... error=none]
    000:<448 hex digits>
    ...
    [FRAME end]

Rows carry their index, so a row damaged on the shared console is simply
left black. Each PNG is also written 3x enlarged (nearest neighbour) for
viewing. Requires pyserial, numpy and matplotlib (for PNG output).
"""
import argparse
import re
import time
from pathlib import Path

import numpy as np
import serial
import serial.tools.list_ports


def find_port():
    for p in serial.tools.list_ports.comports():
        if "303A" in (p.hwid or "").upper():
            return p.device
    raise SystemExit("no Espressif USB-Serial/JTAG port found; pass the port explicitly")


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.dtr = False          # never assert DTR/RTS on the ESP32-C5 USB-Serial/JTAG port
    ser.rts = False
    ser.timeout = 0.2
    ser.open()
    return ser


def grab(ser, timeout_s=10.0):
    ser.reset_input_buffer()
    ser.write(b"g")
    ser.flush()
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        buf += ser.read(65536)
        if b"[FRAME end]" in buf:
            buf += ser.read(1024)
            break
    return bytes(buf)


def parse(text):
    h = re.search(r"\[FRAME ([^\]]*)\]", text)
    if not h:
        return None, None
    meta = dict(re.findall(r"(\w+)=([^\s\]]+(?: \([^)]*\))?)", h.group(1)))
    err = re.search(r"error=([^\]]*)", h.group(1))
    if err:
        meta["error"] = err.group(1)
    w, hgt = int(meta.get("w", 224)), int(meta.get("h", 168))
    img = np.zeros((hgt, w), dtype=np.uint8)
    got = 0
    for m in re.finditer(r"^(\d{3}):([0-9a-fA-F]+)\r?$", text[h.end():], re.M):
        y, hexrow = int(m.group(1)), m.group(2)
        if y < hgt and len(hexrow) == 2 * w:
            img[y] = np.frombuffer(bytes.fromhex(hexrow), dtype=np.uint8)
            got += 1
    meta["rows_parsed"] = got
    return img, meta


def save(img, out, stem):
    (out / f"{stem}.pgm").write_bytes(b"P5\n%d %d\n255\n" % (img.shape[1], img.shape[0]) + img.tobytes())
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        big = np.kron(img, np.ones((3, 3), dtype=np.uint8))
        plt.imsave(out / f"{stem}.png", big, cmap="gray", vmin=0, vmax=255)
        return out / f"{stem}.png"
    except ImportError:
        return out / f"{stem}.pgm"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--out", default="scratch")
    ap.add_argument("--keys", default="", help="console keys to send first (e.g. channel changes)")
    ap.add_argument("--file", help="parse a saved capture instead of talking to the board")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.file:
        img, meta = parse(Path(args.file).read_bytes().decode("ascii", "replace"))
        print(meta)
        if img is not None:
            print("saved", save(img, out, Path(args.file).stem))
        return

    port = args.port or find_port()
    ser = open_port(port)
    try:
        for k in args.keys:
            ser.write(k.encode("ascii"))
            ser.flush()
            time.sleep(0.3)
        for n in range(args.count):
            stamp = time.strftime("%Y%m%d-%H%M%S")
            data = grab(ser)
            (out / f"frame_{stamp}_{n:03d}.txt").write_bytes(data)
            img, meta = parse(data.decode("ascii", "replace"))
            if img is None:
                print(f"frame {n}: no [FRAME] header received")
                continue
            path = save(img, out, f"frame_{stamp}_{n:03d}")
            keys = ("rows", "rows_parsed", "std", "line_us", "fields", "acquire_ms", "grab_ms",
                    "late", "nosync", "sync", "blank", "jitter_ns", "freq", "gain", "error")
            print(f"frame {n}: " + " ".join(f"{k}={meta.get(k)}" for k in keys) + f" -> {path}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
