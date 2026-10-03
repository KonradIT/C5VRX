#!/usr/bin/env python3
"""Fetch one raw I/Q ring snapshot and its BitScrambler-demodulated CVBS from
a C5VRX built with CONFIG_C5VRX_LINK_MODE, then analyse and plot both.

usage: python tools/link_dump.py [PORT] [--out DIR] [--keys KEYS] [--listen S]
       python tools/link_dump.py --file CAPTURE.txt      (re-analyse a saved capture)

The firmware answers the 'v' key with two hex blocks:

    [DUMP raw len=N gain=G freq=F bw40=B]
    <hex, 64 bytes per line>
    [DUMP end raw]
    [DUMP info loopback N -> M bytes in T us]
    [DUMP cvbs len=M ...]
    <hex>
    [DUMP end cvbs]

raw:  one byte per 40 MS/s sample, (I[9:6] << 4) | Q[9:6], two's complement nibbles.
cvbs: the fm.bsasm output, one byte per 40 MHz DAC clock, each 20 MS/s CVBS
      sample repeated twice ([D,D]); 6-bit codes 0..63, pedestal 20.

The console is shared with the receiver's status lines, so the whole byte
stream is captured first (saved as capture_<stamp>.txt) and parsed afterwards;
only full-length pure-hex rows are used (a damaged row is dropped rather than
misplacing the rest). Requires pyserial, numpy and matplotlib.
"""
import argparse
import re
import time
from pathlib import Path

import numpy as np
import serial
import serial.tools.list_ports

IQ_RATE = 40e6
CVBS_RATE = 20e6


def find_port():
    for p in serial.tools.list_ports.comports():
        if "303A" in (p.hwid or "").upper():
            return p.device
    raise SystemExit("no Espressif USB-Serial/JTAG port found; pass the port explicitly")


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.dtr = False          # never assert DTR/RTS: the USB-Serial/JTAG bridge maps them to reset/boot
    ser.rts = False
    ser.timeout = 0.2
    ser.open()
    return ser


def capture(port, keys="", listen_s=0.0, dump=True, timeout_s=25.0):
    """Send keys, optionally show listen_s of console, then 'v' and capture the raw bytes."""
    ser = open_port(port)
    try:
        ser.reset_input_buffer()
        for k in keys:
            ser.write(k.encode("ascii"))
            ser.flush()
            time.sleep(0.3)
        t_end = time.time() + listen_s
        pending = b""
        while time.time() < t_end:
            pending += ser.read(4096)
            *lines, pending = pending.split(b"\n")
            for ln in lines:
                print("  ", ln.decode("ascii", "replace").rstrip())
        if not dump:
            return b""
        ser.reset_input_buffer()
        ser.write(b"v")
        ser.flush()
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < timeout_s:
            buf += ser.read(65536)
            if b"[DUMP end cvbs]" in buf or (b"[DUMP error" in buf and b"\n" in buf[buf.find(b"[DUMP error"):]):
                buf += ser.read(4096)
                break
        return bytes(buf)
    finally:
        ser.close()


def parse(text):
    """Extract the hex blocks from a captured console stream."""
    blocks, meta, info = {}, {}, []
    for m in re.finditer(r"\[DUMP (?:info|error)[^\]]*\]", text):
        info.append(m.group(0))
    for name in ("raw", "cvbs"):
        h = re.search(r"\[DUMP %s len=(\d+)([^\]]*)\]" % name, text)
        e = re.search(r"\[DUMP end %s\]" % name, text)
        if not h or not e or e.start() < h.end():
            continue
        md = dict(re.findall(r"(\w+)=(-?\w+)", h.group(2)))
        md["len"] = int(h.group(1))
        rows, dropped = [], 0
        body = text[h.end():e.start()].splitlines()
        full = []
        for ln in body:
            ln = ln.strip()
            if not ln:
                continue
            if re.fullmatch(r"[0-9a-fA-F]+", ln) and len(ln) % 2 == 0:
                full.append(ln)
            else:
                dropped += 1
        # every row is 128 hex digits except the last one
        for k, ln in enumerate(full):
            if len(ln) == 128 or k == len(full) - 1:
                rows.append(ln)
            else:
                dropped += 1
        data = bytes.fromhex("".join(rows))
        md["dropped_lines"] = dropped
        md["got"] = len(data)
        blocks[name] = np.frombuffer(data, dtype=np.uint8)
        meta[name] = md
    return blocks, meta, info


def unpack_iq(raw):
    q = (raw & 0x0F).astype(np.int16)
    i = (raw >> 4).astype(np.int16)
    q[q >= 8] -= 16
    i[i >= 8] -= 16
    return i, q


def software_demod(i, q, decim=8):
    """Cross product of consecutive samples: proportional to sin(phase step)."""
    cross = i[:-1] * q[1:] - q[:-1] * i[1:]
    n = (len(cross) // decim) * decim
    return cross[:n].reshape(-1, decim).mean(axis=1)


def line_period_estimate(x, rate, lo_us=55.0, hi_us=75.0):
    """Autocorrelation peak between lo_us and hi_us, returned as (period_us, r)."""
    x = np.asarray(x, dtype=np.float64)
    x = x - x.mean()
    if len(x) < 4 or not np.any(x):
        return None, 0.0
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    ac = ac / ac[0]
    lo = int(lo_us * 1e-6 * rate)
    hi = min(int(hi_us * 1e-6 * rate), len(ac) - 1)
    if hi <= lo:
        return None, 0.0
    k = lo + int(np.argmax(ac[lo:hi]))
    return k / rate * 1e6, float(ac[k])


def analyse(blocks, meta, info, out, stamp, plot=True):
    for line in info:
        print("  ", line)
    print("meta:", meta)
    if "raw" not in blocks:
        raise SystemExit("no raw block in the capture")
    raw = blocks["raw"]
    i, q = unpack_iq(raw)
    power = i.astype(np.int32) ** 2 + q.astype(np.int32) ** 2
    print(f"raw: {len(raw)} samples = {len(raw) / IQ_RATE * 1e6:.1f} us, mean power {power.mean():.2f}, "
          f"clipped {np.mean((i == 7) | (i == -8) | (q == 7) | (q == -8)) * 100:.1f} %")
    sw = software_demod(i, q, decim=8)          # 5 MS/s
    per, r = line_period_estimate(sw, IQ_RATE / 8)
    print(f"software demod: line period {per and f'{per:.2f} us'}, autocorrelation {r:.2f} "
          f"(video: ~63.6/64.0 us and r > 0.3)")
    cvbs = None
    if "cvbs" in blocks and len(blocks["cvbs"]) > 16:
        cvbs = blocks["cvbs"][:-8:2].astype(np.float64)   # [D,D]: every second byte; drop the drain tail
        per2, r2 = line_period_estimate(cvbs, CVBS_RATE)
        hist = np.bincount(cvbs.astype(np.int64), minlength=64)
        print(f"bitscrambler cvbs: {len(cvbs)} samples, min {cvbs.min():.0f} max {cvbs.max():.0f} "
              f"mean {cvbs.mean():.1f}, line period {per2 and f'{per2:.2f} us'}, autocorrelation {r2:.2f}")
        print(f"   share at sync tip (code <= 4): {hist[:5].sum() / len(cvbs) * 100:.1f} %, "
              f"at blanking 18..22: {hist[18:23].sum() / len(cvbs) * 100:.1f} %")
    if not plot:
        return
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    t_raw = np.arange(len(raw)) / IQ_RATE * 1e6
    fig, axes = plt.subplots(3 if cvbs is not None else 2, 1, figsize=(15, 9), sharex=True)
    axes[0].plot(t_raw, power, lw=0.4)
    axes[0].set_ylabel("I^2+Q^2")
    axes[0].set_title(f"raw ring snapshot {meta.get('raw', {})}")
    t_sw = (np.arange(len(sw)) * 8 + 4) / IQ_RATE * 1e6
    axes[1].plot(t_sw, sw, lw=0.6)
    axes[1].set_ylabel("software FM (cross)")
    if cvbs is not None:
        t_cv = np.arange(len(cvbs)) / CVBS_RATE * 1e6
        axes[2].plot(t_cv, cvbs, lw=0.5)
        axes[2].set_ylabel("BitScrambler CVBS code")
        axes[2].set_ylim(-1, 64)
    axes[-1].set_xlabel("us")
    for ax in axes:
        for k in range(1, 8):
            ax.axvline(k * 63.56, color="0.85", lw=0.5)   # NTSC line grid, for the eye
    png = out / f"link_{stamp}.png"
    fig.tight_layout()
    fig.savefig(png, dpi=110)
    print("plot:", png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=None)
    ap.add_argument("--out", default="scratch")
    ap.add_argument("--no-plot", action="store_true")
    ap.add_argument("--keys", default="", help="console keys to send first, e.g. 'd' or 'c'")
    ap.add_argument("--listen", type=float, default=0.0, help="seconds of console output to show before the dump")
    ap.add_argument("--no-dump", action="store_true", help="only send keys and listen")
    ap.add_argument("--file", help="analyse a saved capture instead of talking to the board")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    if args.file:
        text = Path(args.file).read_bytes().decode("ascii", "replace")
    else:
        port = args.port or find_port()
        print(f"fetching from {port} ...")
        data = capture(port, keys=args.keys, listen_s=args.listen, dump=not args.no_dump)
        if args.no_dump:
            return
        (out / f"capture_{stamp}.txt").write_bytes(data)
        text = data.decode("ascii", "replace")
    blocks, meta, info = parse(text)
    for name, arr in blocks.items():
        (out / f"link_{stamp}_{name}.bin").write_bytes(arr.tobytes())
    analyse(blocks, meta, info, out, stamp, plot=not args.no_plot)


if __name__ == "__main__":
    main()
