#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture buffer of the MCUViewer bench build, and the two measurements made from it.

The firmware must be built with CONFIG_SSTEP_MOTOR_MCUVIEWER=y (app/mcuviewer.conf).
MCUViewer shows the live variables of `mcuv`; this script starts a capture of the fast
loop at up to 20 kHz over the same SWD probe, saves it as CSV and evaluates it.
Stop the acquisition in MCUViewer first: one program at a time on the ST-LINK.

  python tools/mcuviewer/mcuv.py capture --elf build-mcuviewer/app/zephyr/zephyr.elf \\
      --divider 1 --out rest.csv            # 51 ms at 20 kHz: ripple at rest
  python tools/mcuviewer/mcuv.py ripple rest.csv
  python tools/mcuviewer/mcuv.py capture --elf ... --divider 20 --out slow.csv
  python tools/mcuviewer/mcuv.py deadtime slow.csv      # slow move, 1 s at 1 kHz

capture needs pyOCD (pip install pyocd); ripple and deadtime need only Python.
See docs/measurements.md.
"""

from __future__ import annotations

import argparse
import csv
import math
import struct
import sys
import time
from pathlib import Path

MAGIC = 0x5655434D
VERSION = 1

# struct sstep_mcuviewer (drivers/motor/sstep_mcuviewer.h), offsets checked at build time
OFF = {
    "magic": 0, "version": 4, "pwm_freq_hz": 8, "dead_time_ns": 12, "shared_leg": 16,
    "periods": 20, "vbus_mv": 44, "outputs_on": 48,
    "capture_arm": 80, "capture_divider": 84, "capture_count": 88,
    "capture_length": 92, "capture_rate_hz": 96,
}
POINT_BYTES = 8  # i_a_ma, i_b_ma, v_a_mv, v_b_mv: int16 each


# --------------------------------------------------------------------------- capture

def elf_symbols(elf: Path, names: list[str]) -> dict[str, int]:
    from elftools.elf.elffile import ELFFile  # installed with pyOCD

    found: dict[str, int] = {}
    with elf.open("rb") as f:
        symtab = ELFFile(f).get_section_by_name(".symtab")
        if symtab is None:
            sys.exit(f"{elf}: no symbol table")
        for name in names:
            syms = symtab.get_symbol_by_name(name)
            if not syms:
                sys.exit(f"{elf}: no symbol {name}: build with CONFIG_SSTEP_MOTOR_MCUVIEWER=y "
                         "(-DEXTRA_CONF_FILE=mcuviewer.conf)")
            found[name] = syms[0]["st_value"]
    return found


def cmd_capture(args: argparse.Namespace) -> None:
    try:
        from pyocd.core.helpers import ConnectHelper
    except ImportError:
        sys.exit("pyOCD missing: pip install pyocd")

    sym = elf_symbols(Path(args.elf), ["mcuv", "mcuv_buf"])
    base, buf = sym["mcuv"], sym["mcuv_buf"]
    options = {"connect_mode": "attach", "frequency": args.swd_hz}
    with ConnectHelper.session_with_chosen_probe(unique_id=args.uid,
                                                 target_override=args.target,
                                                 options=options) as session:
        t = session.target

        def rd(name: str) -> int:
            return t.read32(base + OFF[name])

        if rd("magic") != MAGIC or rd("version") != VERSION:
            sys.exit("mcuv not found in the target: is the ELF the one that runs?")
        length = rd("capture_length")
        t.write32(base + OFF["capture_arm"], 0)
        t.write32(base + OFF["capture_divider"], max(args.divider, 1))
        t.write32(base + OFF["capture_arm"], 1)

        pwm = rd("pwm_freq_hz")
        timeout = 2.0 + length * max(args.divider, 1) / pwm
        end = time.monotonic() + timeout
        while rd("capture_arm") != 0:
            if time.monotonic() > end:
                sys.exit(f"capture not finished after {timeout:.1f} s: does the fast loop run?")
            time.sleep(0.02)

        count = rd("capture_count")
        raw = bytes(t.read_memory_block8(buf, count * POINT_BYTES))
        meta = {
            "pwm_freq_hz": pwm,
            "rate_hz": rd("capture_rate_hz"),
            "dead_time_ns": rd("dead_time_ns"),
            "shared_leg": rd("shared_leg"),
            "vbus_mv": struct.unpack("<i", struct.pack("<I", rd("vbus_mv")))[0],
            "outputs_on": rd("outputs_on"),
        }

    points = list(struct.iter_unpack("<hhhh", raw))
    write_csv(Path(args.out), meta, points)
    print(f"{len(points)} points at {meta['rate_hz']} Hz, Vbus {meta['vbus_mv']} mV -> {args.out}")
    if not meta["outputs_on"]:
        print("note: the outputs were off (Torque Enable = 0): currents and voltages are 0")


def write_csv(path: Path, meta: dict[str, int], points: list[tuple[int, ...]]) -> None:
    with path.open("w", newline="") as f:
        for k, v in meta.items():
            f.write(f"# {k}={v}\n")
        w = csv.writer(f)
        w.writerow(["t_s", "i_a_ma", "i_b_ma", "v_a_mv", "v_b_mv"])
        for n, p in enumerate(points):
            w.writerow([f"{n / meta['rate_hz']:.6f}", *p])


def read_csv(path: Path) -> tuple[dict[str, int], list[list[float]]]:
    meta: dict[str, int] = {}
    rows: list[list[float]] = []
    with path.open() as f:
        for line in f:
            if line.startswith("#"):
                k, v = line[1:].strip().split("=", 1)
                meta[k] = int(v)
            elif line[:1].isdigit() or line[:1] == "-":
                rows.append([float(x) for x in line.strip().split(",")])
    if not rows:
        sys.exit(f"{path}: no data")
    return meta, rows


# --------------------------------------------------------------------------- ripple

def dominant_hz(x: list[float], rate: float) -> tuple[float, float]:
    """Frequency and amplitude of the largest line of the AC part (plain DFT)."""
    n = len(x)
    mean = sum(x) / n
    best = (0.0, 0.0)
    for k in range(1, n // 2):
        re = im = 0.0
        w = 2.0 * math.pi * k / n
        for m, v in enumerate(x):
            re += (v - mean) * math.cos(w * m)
            im -= (v - mean) * math.sin(w * m)
        amp = 2.0 * math.hypot(re, im) / n
        if amp > best[1]:
            best = (k * rate / n, amp)
    return best


def cmd_ripple(args: argparse.Namespace) -> None:
    meta, rows = read_csv(Path(args.csv))
    rate = meta["rate_hz"]
    print(f"{len(rows)} points, {len(rows) / rate * 1000:.1f} ms at {rate} Hz")
    for name, col in (("A", 1), ("B", 2)):
        x = [r[col] for r in rows]
        mean = sum(x) / len(x)
        std = math.sqrt(sum((v - mean) ** 2 for v in x) / len(x))
        f, amp = dominant_hz(x, rate)
        print(f"phase {name}: mean {mean:8.1f} mA  min {min(x):6.0f}  max {max(x):6.0f}  "
              f"p-p {max(x) - min(x):5.0f} mA  rms {std:6.1f} mA  "
              f"largest line {f:7.1f} Hz, {amp:6.1f} mA")


# --------------------------------------------------------------------------- dead time

def solve(a: list[list[float]], b: list[float]) -> list[float]:
    """Gaussian elimination with partial pivoting."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            raise ValueError("singular")
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [x - f * y for x, y in zip(m[r], m[c])]
    return [m[i][n] / m[i][i] for i in range(n)]


def sign(x: float) -> float:
    return (x > 0) - (x < 0)


def phase_samples(meta: dict[str, int], rows: list[list[float]], lim: float):
    """(phase, i [A], v [V], s) of every phase sample with |i| >= lim mA.

    Every leg loses Vd against the direction of its current. Shared leg: phase A is
    U - W, the current leaves U with i_a and W with -(i_a + i_b), so
    s_A = sign(i_a) + sign(i_a + i_b); full bridge: s_A = 2 sign(i_a).
    """
    shared = meta.get("shared_leg", 1) == 1
    for _, ia, ib, va, vb in rows:
        for ph, (i, v) in enumerate(((ia, va), (ib, vb))):
            if abs(i) < lim or (shared and abs(ia + ib) < lim):
                continue
            s = (sign(i) + sign(ia + ib)) if shared else 2.0 * sign(i)
            yield ph, i / 1000.0, v / 1000.0, s


def cmd_deadtime(args: argparse.Namespace) -> None:
    """Fit v = R i + Vd s + c per phase; Vd = Vbus t_eff f_pwm is the loss of one leg.

    With --r-ohm, R is fixed (measured with an ohmmeter) and only Vd and the offsets are
    fitted. The table after the fit shows the loss per current range: a dead time whose
    loss grows with the current shows up there, and as a larger R in the free fit.
    """
    meta: dict[str, int] = {}
    rows: list[list[float]] = []
    for path in args.csv:
        m, r = read_csv(Path(path))
        meta = meta or m
        rows += r
    samples = list(phase_samples(meta, rows, args.min_ma))
    pos = sum(1 for _, i, _, _ in samples if i > 0)
    neg = len(samples) - pos
    if pos < 20 or neg < 20:
        sys.exit(f"too few points with both current directions (+{pos} / -{neg}): capture "
                 "a slow move over at least one electrical period (4 full steps)")

    fixed_r = args.r_ohm
    n = 3 if fixed_r is not None else 4
    xtx = [[0.0] * n for _ in range(n)]
    xty = [0.0] * n
    for ph, i, v, s in samples:
        x = [s, 1.0 if ph == 0 else 0.0, 1.0 if ph == 1 else 0.0]
        y = v
        if fixed_r is None:
            x.append(i)
        else:
            y -= fixed_r * i
        for r in range(n):
            xty[r] += x[r] * y
            for c in range(n):
                xtx[r][c] += x[r] * x[c]
    try:
        sol = solve(xtx, xty)
    except ValueError:
        sys.exit("fit failed: the currents do not vary enough (move the motor slowly)")
    vd, ca, cb = sol[:3]
    r_ohm = fixed_r if fixed_r is not None else sol[3]

    vbus = meta["vbus_mv"] / 1000.0
    f_pwm = meta["pwm_freq_hz"]

    def t_ns(loss_v: float) -> float:
        return loss_v / (vbus * f_pwm) * 1e9

    res = sum((v - (r_ohm * i + vd * s + (ca if ph == 0 else cb))) ** 2
              for ph, i, v, s in samples)
    rms = math.sqrt(res / len(samples))

    print(f"{len(samples)} phase samples with |i| >= {args.min_ma:.0f} mA (+{pos} / -{neg}), "
          f"Vbus {vbus:.2f} V, PWM {f_pwm} Hz")
    print(f"R phase        {r_ohm:7.3f} Ohm   ({'given' if fixed_r is not None else 'fitted'}:"
          " coil, cables, MOSFETs, shunt)")
    print(f"Vd per leg     {vd * 1000:7.0f} mV")
    print(f"offset A / B   {ca * 1000:7.0f} / {cb * 1000:.0f} mV")
    print(f"residual rms   {rms * 1000:7.0f} mV")
    print(f"effective dead time {t_ns(vd):6.0f} ns   (set: {meta.get('dead_time_ns', 0)} ns)")

    # Loss per current range, from the samples where both legs carry the same sign
    bins: dict[int, list[float]] = {}
    for ph, i, v, s in samples:
        if abs(s) == 2.0:
            k = int(abs(i) * 1000 // args.bin_ma)
            bins.setdefault(k, []).append((v - r_ohm * i - (ca if ph == 0 else cb)) / s)
    print(f"\n|i| range [mA]   samples   Vd per leg [mV]   dead time [ns]   (R = {r_ohm:.3f} Ohm)")
    for k in sorted(bins):
        if len(bins[k]) < 10:
            continue
        m = sum(bins[k]) / len(bins[k])
        lo, hi = k * args.bin_ma, (k + 1) * args.bin_ma
        print(f"{lo:6.0f} .. {hi:<6.0f} {len(bins[k]):8d} {m * 1000:15.0f} {t_ns(m):16.0f}")


# --------------------------------------------------------------------------- main

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="record the fast loop into a CSV file")
    c.add_argument("--elf", required=True, help="zephyr.elf of the running bench build")
    c.add_argument("--out", required=True, help="CSV file")
    c.add_argument("--divider", type=int, default=1,
                   help="record every n-th PWM period (1 = 20 kHz)")
    c.add_argument("--uid", default=None, help="probe serial number (pyocd list)")
    c.add_argument("--target", default="cortex_m",
                   help="pyOCD target; cortex_m is enough for memory access")
    c.add_argument("--swd-hz", type=int, default=4000000)
    c.set_defaults(fn=cmd_capture)

    r = sub.add_parser("ripple", help="statistics of the phase currents of a capture")
    r.add_argument("csv")
    r.set_defaults(fn=cmd_ripple)

    d = sub.add_parser("deadtime", help="effective dead time from a capture of a slow move")
    d.add_argument("csv", nargs="+", help="one or more captures at the same bus voltage")
    d.add_argument("--r-ohm", type=float, default=None,
                   help="phase resistance measured with an ohmmeter; fitted when omitted")
    d.add_argument("--bin-ma", type=float, default=250.0,
                   help="current range of one row of the loss table")
    d.add_argument("--min-ma", type=float, default=150.0,
                   help="leave out currents near zero, where the loss is not yet full")
    d.set_defaults(fn=cmd_deadtime)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
