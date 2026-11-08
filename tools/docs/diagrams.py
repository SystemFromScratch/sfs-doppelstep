#******************************************************************************
# Doppelstep: Zephyr RTOS stepper motor controller seen as a DYNAMIXEL servo
#
# Description: Generates the illustrations of the documentation
#              (docs/assets/images/*.svg). Run after changing one:
#                  python tools/docs/diagrams.py
#
# Author:      Anton Vynohradov
# Email:       avynohradov@systemfromscratch.com
# License:     Apache License 2.0
# Copyright (c) 2026 Anton Vynohradov
#
# SPDX-License-Identifier: Apache-2.0
#*****************************************************************************

"""Documentation illustrations of Doppelstep, built with svgkit.

hero-3d.svg shows what makes Doppelstep different: DYNAMIXEL Protocol 2.0 packets from
the host reach a controller board, the firmware turns them into two phase currents 90°
apart (sine and cosine, Stepper-C microstepping) through the three half-bridges of the
shared-leg stage, and a NEMA 17 stepper with a servo horn turns like the servo the host
expects. Preview the SVG in a browser after a change and check every label."""

from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from svgkit import BLUE, HERO_DEFS, HERO_STYLE, Iso  # noqa: E402

OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                    "docs", "assets", "images"))

HERO_CHIP = ("Doppelstep", "Zephyr RTOS")
HOST_LABEL = "ROS 2"
BUS_LABEL = "PROTOCOL 2.0"
PHASE_A = "#5ea6ff"
PHASE_B = "#3fd0c9"

# Rotating horn: animateTransform inside the top-face matrix, so it turns in the plane
# of the motor's top face. Packets: animateMotion along the bus, hidden in print.
HERO_EXTRA_STYLE = (
    ".horn{transform-box:view-box}"
    "@media (prefers-reduced-motion:reduce){.packet{display:none}}"
)


def hero() -> None:
    iso = Iso()
    p = iso.p

    # Host connector: the bus arrives from the host (ROS 2, DynamixelSDK, Wizard)
    iso.box(-7.0, -4.6, 2.6, 4.6, -0.4, 1.3, "#1b2a40", "#131f31", "#0e1726", "#2a4870")
    iso.text(-5.8, 4.6, 0.25, HOST_LABEL, 0.5, "#9aabc4", 700, face="y")
    iso.led(-5.0, 4.6, 1.0, "led-rx", r=2.6)
    bus = [p(-4.6, 3.6, 0.35), p(-2.2, 3.6, 0.35), p(0.0, 3.6, 0.35)]
    iso.path(bus, "#0e2a4f", 6)
    iso.path(bus, "#2f8cff", 2.0, ' opacity="0.9"')
    iso.text(-2.3, 3.0, 0.4, BUS_LABEL, 0.3, "#6b7d99", 600, 0.05)

    # Board with traces
    iso.box(0, 10, 0, 7.2, -0.4, 0, "url(#boardTop)", "#0a1628", "#071122", "#1e3a66", 1.2)
    for i in range(6):
        y = 2.3 + i * 0.55
        iso.path([p(2.0, y), p(1.1, y), p(0.7, y - 0.4), p(0.25, y - 0.4)], "#1d5fbf", 1.3,
                 ' opacity="0.7"')
    for i in range(5):
        x = 2.4 + i * 0.6
        iso.path([p(x, 5.4), p(x, 6.2), p(x - 0.3, 6.5), p(x - 0.3, 6.95)], "#1d5fbf", 1.3,
                 ' opacity="0.7"')

    # Gate signals from the chip to the three half-bridges (legs U, V, W)
    legs = [1.0, 2.9, 4.8]
    for y in legs:
        trace = [p(5.4, 3.6), p(6.0, 3.6), p(6.4, y + 0.6), p(6.9, y + 0.6)]
        iso.path(trace, "#1d5fbf", 1.5)
        iso.path(trace, "#5ea6ff", 1.5, ' class="flow"')
    for y in legs:
        iso.box(6.9, 8.4, y, y + 1.2, 0, 0.5, "#14243c", "#0c182b", "#09121f", "#2a4870")
        iso.box(7.15, 8.15, y + 0.2, y + 1.0, 0.5, 0.58, "#8fa6c6", "#6b819f", "#5a6f8b")
    for y in legs:
        iso.path([p(8.4, y + 0.6), p(9.1, y + 0.6), p(9.2, 3.6)], "#1d5fbf", 1.5,
                 ' opacity="0.8"')
    iso.box(9.2, 10.0, 2.6, 4.6, 0, 0.55, "#c9d6ea", "#8aa0bf", "#6f86a6")

    # Chip: the firmware on the STM32G431
    for i in range(7):
        c = 2.25 + i * 0.45
        iso.box(c - 0.08, c + 0.08, 5.2, 5.45, 0, 0.12, "#8fa6c6", "#6b819f", "#5a6f8b")
        iso.box(5.2, 5.45, c - 0.08, c + 0.08, 0, 0.12, "#8fa6c6", "#6b819f", "#5a6f8b")
    iso.box(2.0, 5.2, 2.0, 5.2, 0, 0.75, "url(#chipTop)", "#0a1322", "#060d18", "#2f8cff", 1.0)
    iso.text(3.6, 3.55, 0.76, HERO_CHIP[0], 0.5, "#e6edf7", 800, 0.02)
    iso.text(3.6, 4.25, 0.76, HERO_CHIP[1].upper(), 0.26, "#5ea6ff", 600, 0.06)
    iso.led(2.45, 2.45, 0.76)

    # Motor cable with the two phase currents: A = sine, B = cosine (90° apart)
    dx0, dx1, dy0, dy1 = 17.0, 21.0, 1.6, 5.6
    cable = [p(10.0, 3.6, 0.3), p(dx0, 3.6, 0.3)]
    iso.path(cable, "#0e2a4f", 6)
    iso.path(cable, BLUE, 2.2)
    for color, phase, z0 in ((PHASE_A, 0.0, 1.55), (PHASE_B, math.pi / 2, 1.55)):
        wave = []
        for n in range(121):
            t = n / 120
            wave.append(p(10.6 + t * (dx0 - 11.2), 3.6,
                          z0 + 0.6 * math.sin(2 * math.pi * 2.5 * t + phase)))
        iso.path(wave, color, 2.4, ' opacity="0.45" filter="url(#glow)"')
        iso.path(wave, color, 2.0, ' class="flow"')

    # NEMA 17 stepper with a servo horn on the shaft
    iso.box(dx0, dx1, dy0, dy1, -0.4, 0.9, "#1b2a40", "#131f31", "#0e1726", "#2a4870")
    iso.box(dx0, dx1, dy0, dy1, 0.9, 1.15, "#2a4870", "#1e3a66", "#18304f", "#2a4870")
    iso.box(dx0, dx1, dy0, dy1, 1.15, 3.0, "#1b2a40", "#131f31", "#0e1726", "#2a4870")
    cx, cy = (dx0 + dx1) / 2, (dy0 + dy1) / 2
    iso.disc(cx, cy, 3.0, 1.25, [(1.0, "#8aa0bf"), (0.85, "#c9d6ea"), (0.25, "#8aa0bf")])
    ox, oy = p(cx, cy, 3.02)
    c, s = iso.cos30 * iso.scale, 0.5 * iso.scale
    iso.parts.append(
        f'<g transform="matrix({c:.3f} {s:.3f} {-c:.3f} {s:.3f} {ox:.1f} {oy:.1f})">'
        '<g><rect x="-0.16" y="-1.05" width="0.32" height="2.1" rx="0.16" fill="#2f8cff"/>'
        '<circle cx="0" cy="-0.85" r="0.09" fill="#e6edf7"/>'
        '<animateTransform attributeName="transform" type="rotate" values="0;90;90;-30;-30;0" '
        'keyTimes="0;0.3;0.45;0.75;0.9;1" dur="5s" repeatCount="indefinite"/></g></g>')
    iso.led(dx1 - 0.5, dy1, 2.5, r=2.6)

    # Packets travelling from the host to the board (hidden until the animation runs)
    start, stop = p(-4.4, 3.6, 0.55), p(-0.2, 3.6, 0.55)
    track = f"M{start[0]:.1f},{start[1]:.1f} L{stop[0]:.1f},{stop[1]:.1f}"
    for color, glow, motion, visible in (("#2f8cff", "#9cc8ff", "0;1;1;1", "0;1;1;0;0;0"),
                                         ("#3fcf8e", "#a6f0cb", "1;1;1;0", "0;0;0;1;1;0")):
        cubes = "".join(f'<rect x="{-15 + k * 10}" y="-4" width="8" height="8" rx="1.5" '
                        f'fill="{color}" stroke="{glow}" stroke-width="0.8" transform="skewY(30)"/>'
                        for k in range(3))
        iso.parts.append(
            f'<g class="packet" opacity="0" filter="url(#glow)">{cubes}'
            f'<animateMotion dur="5s" repeatCount="indefinite" path="{track}" '
            f'keyPoints="{motion}" keyTimes="0;0.35;0.55;1" calcMode="linear"/>'
            f'<animate attributeName="opacity" dur="5s" repeatCount="indefinite" '
            f'values="{visible}" keyTimes="0;0.05;0.33;0.4;0.95;1"/></g>')

    path = os.path.join(OUT, "hero-3d.svg")
    label = ("Doppelstep: DYNAMIXEL Protocol 2.0 packets from a ROS 2 host reach the "
             "controller board, which drives a stepper motor with two phase currents "
             "90 degrees apart")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(iso.svg(label, HERO_DEFS, HERO_STYLE + HERO_EXTRA_STYLE, glow_at=(8, 4)))


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    hero()
    for name in sorted(os.listdir(OUT)):
        if name.endswith(".svg"):
            print("wrote", os.path.join("docs", "assets", "images", name))


if __name__ == "__main__":
    main()
