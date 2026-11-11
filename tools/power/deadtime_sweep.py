#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Dead time of the B-G431B-ESC1 power stage from an ngspice sweep.

The method is described in docs/dead-time.md. This tool reuses the models and
helpers of sfs-stepper-power-spice (L6387 driver, STL180N6F7, one leg) and adds the
parts of the MB1419 rev C gate circuit the leg model does not contain yet: the
10 kOhm gate-source resistors and the 1 Ohm + 10 nF snubber on the phase node.

    python tools/power/deadtime_sweep.py sweep      # about 15 minutes, writes the CSV
    python tools/power/deadtime_sweep.py plot       # figures for docs/dead-time.md
    python tools/power/deadtime_sweep.py schematic --pdf mb1419-g431cbu6-c01_schematic.pdf

Requirements: ngspice in PATH (or NGSPICE=<path>), numpy, matplotlib; pdftoppm and
Pillow for `schematic`. The model repository is found at
../modules/lib/sfs-stepper-power-spice (west workspace) or with --spice / SFS_POWER_SPICE.
"""

import argparse
import csv
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "deadtime")
IMG = os.path.join(ROOT, "docs", "assets", "images", "dead-time")

# Board values read from schematic MB1419 rev C, sheet 5 (POWER); see docs/dead-time.md
BOARD = dict(
    vbus=24.0,      # V, laboratory supply
    fs=20000.0,     # Hz, PWM frequency (pwm-frequency in the overlay)
    vcc=10.0,       # V, +10V rail of the L7986 (R30/R33), supply of the L6387ED
    rg=33.0,        # Ohm, R34/R38/R40/R43/R47/R50, no turn-off diode
    rgs=10e3,       # Ohm, R35/R39/R41/R45/R48/R52, gate to source
    cboot=1e-6,     # F, C43/C52/C58
    rsh=3e-3,       # Ohm, R54/R55/R56
    rsn=1.0,        # Ohm, R37/R44/R51, snubber
    csn=10e-9,      # F, C51/C59/C65, snubber
    lsh=1e-9,       # H, shunt and layout (model assumption)
    lloop=3e-9,     # H, commutation loop (model assumption)
)
VTH = 3.0                                   # V, VGS(th) used as the criterion
CURRENTS = (-3.0, -0.25, 0.25, 3.0)         # A, phase current, positive out of the leg
DEAD_NS = tuple(range(300, 1600, 100))
TJ = (25.0, 100.0)
CSV = os.path.join(OUT, "sweep.csv")


def spice_tools(path):
    path = path or os.environ.get("SFS_POWER_SPICE") or os.path.join(
        os.path.dirname(ROOT), "modules", "lib", "sfs-stepper-power-spice")
    tools = os.path.join(path, "tools")
    if not os.path.isfile(os.path.join(tools, "run_tests.py")):
        sys.exit(f"sfs-stepper-power-spice not found at {path}: use --spice or SFS_POWER_SPICE")
    sys.path.insert(0, tools)
    import run_tests as rt  # noqa: E402  (the model repository's test runner)
    rt.BUILD = os.path.join(OUT, "spice")
    return rt


def bench(rt, name, b, iload, tmax):
    """One leg at d = 0.5 for 6 periods, as SP02 of sfs-stepper-power-spice, plus the
    gate-source resistors and the snubber. Returns t, v(out), vgs high, vgs low, i bus."""
    ts = 1.0 / b["fs"]
    td = b["dead_ns"] * 1e-9
    d = 0.5
    t1 = ts / 2 * (1 - d)
    t2 = ts / 2 * (1 + d)
    net = f"""* {name}
{rt.includes()}
VBUS bus 0 {b['vbus']}
VCC vcc 0 {b['vcc']}
VH hin 0 PULSE(0 3.3 {t1 + td} 2n 2n {t2 - t1 - td - 2e-9} {ts})
VLP lp 0 PULSE(0 3.3 {t1} 2n 2n {t2 + td - t1 - 2e-9} {ts})
BLIN lin 0 V = 3.3 - V(lp)
X1 bus 0 out hin lin vcc sh ESC1_LEG params: rg={b['rg']} cboot={b['cboot']} rsh={b['rsh']}
+ lsh={b['lsh']} lloop={b['lloop']} tj={b['tj']}
RGSH x1.gh out {b['rgs']}
RGSL x1.gl x1.sl {b['rgs']}
RSN out sn {b['rsn']}
CSN sn 0 {b['csn']}
IL out 0 DC {iload}
.options abstol=1e-9 vntol=1e-5
.control
tran 1n {6 * ts} {4 * ts} {tmax}
let vgsh = v(x1.gh) - v(out)
let vgsl = v(x1.gl) - v(x1.sl)
let ibus = -i(vbus)
wrdata @DAT@ v(out) vgsh vgsl ibus
.endc
.end
"""
    for step in (tmax, "1n", "0.5n"):          # a smaller step when the solver stalls
        try:
            _, data = rt.run(name, net.replace(f" {tmax}\n", f" {step}\n", 1))
            return data[:, 0], data[:, 1], data[:, 3], data[:, 5], data[:, 7]
        except SystemExit:
            continue
    return None


def point(rt, dead, tj):
    """Worst case over CURRENTS of one dead time and temperature."""
    b = dict(BOARD, dead_ns=float(dead), tj=tj)
    ts = 1.0 / b["fs"]
    gap, bump, ipk = float("inf"), 0.0, 0.0
    for i in CURRENTS:
        r = bench(rt, f"dt{dead}_tj{tj:.0f}_i{i:+.2f}", b, i, "2n")
        if r is None:
            return None
        t, _, vh, vl, ib = r
        sel = t >= t[-1] - ts
        g, bm = rt.gate_timing(t[sel], vh[sel], vl[sel], VTH)
        gap = min(gap, g if g == g else -1.0)  # nan: the gates overlap
        bump = max(bump, bm)
        ipk = max(ipk, float(ib[sel].max()))
    return gap, bump, ipk


def cmd_sweep(args):
    rt = spice_tools(args.spice)
    os.makedirs(OUT, exist_ok=True)
    rows = []
    print("dead_ns  tj_c  gate_gap_ns  bump_v  ibus_peak_a")
    for tj in TJ:
        for dead in DEAD_NS:
            res = point(rt, dead, tj)
            if res is None:
                print(f"{dead:7d} {tj:5.0f}  ngspice did not converge", flush=True)
                continue
            gap, bump, ipk = res
            gap_ns = str(round(gap * 1e9)) if gap >= 0 else "overlap"
            rows.append(dict(dead_ns=dead, tj_c=tj, gate_gap_ns=gap_ns,
                             bump_v=round(bump, 2), ibus_peak_a=round(ipk, 1)))
            print(f"{dead:7d} {tj:5.0f} {gap_ns:>12} {bump:7.2f} {ipk:12.1f}", flush=True)
    with open(CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"written {os.path.relpath(CSV, ROOT)}")


# ------------------------------------------------------------------------------
# figures (reference light palette of the documentation)
# ------------------------------------------------------------------------------

SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ("#2a78d6", "#eb6834")             # categorical slots 1 and 2
LIMIT = "#e34948"                           # reference line: VGS(th), shoot-through zone


def style(plt):
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2,
        "ytick.color": INK2, "text.color": INK, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
        "font.size": 10, "axes.titlesize": 11, "axes.titleweight": "bold",
        "axes.titlelocation": "left", "lines.linewidth": 2, "legend.frameon": False,
    })


def plot_sweep(plt):
    with open(CSV) as f:
        rows = list(csv.DictReader(f))
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 3.8), sharex=True)
    for k, tj in enumerate(TJ):
        sel = [r for r in rows if float(r["tj_c"]) == tj]
        x = [int(r["dead_ns"]) for r in sel]
        lab = f"Tj {tj:.0f} °C"
        a1.plot(x, [float(r["bump_v"]) for r in sel], "-o", ms=4, color=SERIES[k], label=lab)
        a2.plot(x, [float(r["ibus_peak_a"]) for r in sel], "-o", ms=4, color=SERIES[k],
                label=lab)
    a1.axhline(VTH, color=LIMIT, lw=1.2, ls="--")
    a1.text(1500, VTH + 0.06, "VGS(th) 3 V", color=INK2, ha="right", va="bottom")
    a1.set_title("Gate of the off switch at the edge")
    a1.set_ylabel("peak VGS [V]")
    a2.set_title("Peak current from the bus")
    a2.set_ylabel("peak i_bus [A], load 3 A")
    a2.set_yscale("log")
    for a in (a1, a2):
        a.set_xlabel("timer dead time [ns]")
        a.axvline(900, color=INK2, lw=1, ls=":")
        a.axvline(1200, color=INK, lw=1.2)
    a1.text(1210, a1.get_ylim()[1], "1200 ns", va="top", fontsize=9)
    a1.text(890, a1.get_ylim()[1], "900 ns", va="top", ha="right", fontsize=9, color=INK2)
    a1.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "sweep.png"), dpi=150)


def plot_edge(plt, rt):
    """VGS of both switches and the bus current around the turn-on of the high side,
    with 500 ns and 1200 ns, at -3 A (the low-side body diode conducts in the dead time)."""
    fig, axes = plt.subplots(2, 2, figsize=(10, 5.2), sharex="col", sharey="row")
    for col, dead in enumerate((500, 1200)):
        b = dict(BOARD, dead_ns=float(dead), tj=25.0)
        t, _, vh, vl, ib = bench(rt, f"edge_dt{dead}", b, -3.0, "1n")
        ts = 1.0 / b["fs"]
        t0 = t[-1] - ts + ts / 4                      # timer edge: low side off (d = 0.5)
        sel = (t >= t0 - 200e-9) & (t <= t0 + dead * 1e-9 + 800e-9)
        x = (t[sel] - t0) * 1e9
        ax, ai = axes[0, col], axes[1, col]
        ax.plot(x, vl[sel], color=SERIES[0], label="low side VGS")
        ax.plot(x, vh[sel], color=SERIES[1], label="high side VGS")
        ax.axhline(VTH, color=LIMIT, lw=1.2, ls="--")
        ai.plot(x, ib[sel], color=SERIES[0])
        for a in (ax, ai):
            a.axvline(0, color=INK2, lw=1, ls=":")
            a.axvline(dead, color=INK2, lw=1, ls=":")
        ax.set_title(f"Timer dead time {dead} ns")
        ai.set_xlabel("time from the low-side command off [ns]")
    axes[0, 0].set_ylabel("VGS [V]")
    axes[1, 0].set_ylabel("i_bus [A]")
    axes[0, 0].text(axes[0, 0].get_xlim()[0] + 20, VTH + 0.2, "VGS(th)", color=INK2)
    axes[0, 1].legend(loc="center right")
    fig.tight_layout()
    fig.savefig(os.path.join(IMG, "edge.png"), dpi=150)


def cmd_plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    if not os.path.isfile(CSV):
        sys.exit(f"{os.path.relpath(CSV, ROOT)} missing: run `sweep` first")
    os.makedirs(IMG, exist_ok=True)
    style(plt)
    plot_sweep(plt)
    plot_edge(plt, spice_tools(args.spice))
    print(f"written {os.path.relpath(IMG, ROOT)}/sweep.png, edge.png")


# ------------------------------------------------------------------------------
# schematic excerpts (MB1419 rev C, sheet 5), positions in points of the A3 page
# ------------------------------------------------------------------------------

CROPS = {
    # name: (x0, y0, x1, y1) in 1/72 inch of the page, areas outside the leg masked
    "leg-u.png": ((480, 240, 1250, 475), [(480, 240, 560, 262), (480, 434, 900, 475),
                                       (972, 463, 1020, 475)]),
    "aux-10v.png": ((20, 0, 560, 255), []),
    "shunt.png": ((0, 655, 445, 775), []),
}


def cmd_schematic(args):
    from PIL import Image, ImageDraw
    os.makedirs(OUT, exist_ok=True)
    dpi = 300
    base = os.path.join(OUT, "mb1419-p5")
    subprocess.run(["pdftoppm", "-f", "5", "-l", "5", "-r", str(dpi), "-png", "-singlefile",
                    args.pdf, base], check=True, capture_output=True)
    page = Image.open(base + ".png").convert("RGB")
    k = page.size[0] / 1289.0                # page width of the A3 sheet in points
    os.makedirs(IMG, exist_ok=True)
    for name, (box, masks) in CROPS.items():
        img = page.copy()
        d = ImageDraw.Draw(img)
        bg = img.getpixel((int(box[0] * k) + 5, int(box[1] * k) + 5))
        for m in masks:
            d.rectangle([int(v * k) for v in m], fill=bg)
        img.crop(tuple(int(v * k) for v in box)).save(os.path.join(IMG, name), optimize=True)
    print(f"written {os.path.relpath(IMG, ROOT)}/" + ", ".join(CROPS))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--spice", help="path of sfs-stepper-power-spice")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sweep", help="run the sweep, write build/deadtime/sweep.csv")
    sub.add_parser("plot", help="figures for docs/dead-time.md")
    s = sub.add_parser("schematic", help="excerpts of schematic MB1419 rev C, sheet 5")
    s.add_argument("--pdf", required=True, help="mb1419-g431cbu6-c01_schematic.pdf from st.com")
    args = p.parse_args()
    {"sweep": cmd_sweep, "plot": cmd_plot, "schematic": cmd_schematic}[args.cmd](args)


if __name__ == "__main__":
    main()
