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

from svgkit import (BLUE, GREEN, GREY, HERO_DEFS, HERO_STYLE, LIGHT, LINE, NAVY,  # noqa: E402
                    ORANGE, TEAL, WHITE, Iso, Svg)

SOFT_BLUE = "#EAF1FB"
SOFT_TEAL = "#E6F4F5"
SOFT_ORANGE = "#FFF7EC"
SOFT_GREEN = "#EAF5EA"

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


# ==============================================================================
# 2D DIAGRAMS (drawn from the code: app/src, drivers/, app_msg.h, the devicetree)
# ==============================================================================

def box(s: Svg, x, y, w, h, title, lines=(), fill=LIGHT, stroke=LINE, color=NAVY,
        size=15, mono_lines=False, dash=False) -> None:
    """A rounded box with a bold title and smaller lines below it."""
    s.rect(x, y, w, h, fill=fill, stroke=stroke, rx=10, dash=dash)
    top = y + h / 2 - (len(lines) * 17) / 2 + (5 if not lines else -2)
    s.text(x + w / 2, top, title, size=size, weight="bold", fill=color)
    for i, value in enumerate(lines):
        s.text(x + w / 2, top + 20 + i * 17, value, size=12.5, fill=GREY, mono=mono_lines)


def arrow(s: Svg, x1, y1, x2, y2, color=GREY, label="", dx=0, dy=-8, size=12, dash=False,
          anchor="middle") -> None:
    kind = {TEAL: "teal", ORANGE: "orange", GREEN: "green"}.get(color, "end")
    s.line(x1, y1, x2, y2, color=color, arrow=kind, dash=dash)
    if label:
        s.text((x1 + x2) / 2 + dx, (y1 + y2) / 2 + dy, label, size=size, fill=color,
               anchor=anchor)


def architecture() -> None:
    """Layers of the firmware: host, drivers, application threads and queues, Stepper-C,
    power stage backends, board. Matches docs/architecture.md and app/src/app_msg.h."""
    s = Svg(1000, 760, "Doppelstep architecture: from the host to the motor")

    # Host
    box(s, 40, 24, 340, 66, "Host PC", ["ROS 2 · DynamixelSDK · DYNAMIXEL Wizard"],
        fill=LIGHT, stroke=GREY)

    # Firmware frame, then the bus arrow on top of it
    s.rect(20, 140, 960, 480, fill=WHITE, stroke=BLUE, rx=14, sw=2)
    s.text(300, 166, "Firmware on Zephyr RTOS v4.5.0", size=16, weight="bold", fill=BLUE,
           anchor="start")
    arrow(s, 155, 90, 155, 188, color=NAVY)
    s.text(167, 120, "DYNAMIXEL Protocol 2.0, 1 Mbps, UART", size=12, fill=NAVY,
           anchor="start")
    s.text(960, 166, "application (app/) in blue · drivers (drivers/) in teal", size=12,
           fill=GREY, anchor="end")

    # Row A: servo driver, comm_task, queues, control_task
    box(s, 40, 190, 230, 104, "X430 servo driver",
        ["X430 emulator + DXL2-C", "child node of the UART", "Control Table, NVS"],
        fill=SOFT_TEAL, stroke=TEAL, color=TEAL)
    arrow(s, 270, 242, 336, 242, color=TEAL, label="callbacks", dy=-8)
    box(s, 338, 190, 220, 104, "comm_task + joint_bridge",
        ["priority 2", "on receive, every 10 ms", "servo units ↔ motor units"],
        fill=SOFT_BLUE, stroke=BLUE, color=BLUE)
    box(s, 760, 190, 200, 104, "control_task",
        ["priority −2, cooperative", "1 kHz, absolute schedule", "the only caller of the motor"],
        fill=SOFT_BLUE, stroke=BLUE, color=BLUE)
    for i, (name, forward, depth) in enumerate((("cmd_q", True, "commands, depth 2"),
                                                 ("rsp_q", False, "results, depth 2"),
                                                 ("status_q", False, "latest status, depth 1"))):
        y = 214 + i * 30
        if forward:
            arrow(s, 562, y, 756, y, color=NAVY)
        else:
            arrow(s, 756, y, 562, y, color=NAVY)
        s.text(659, y - 6, f"{name} · {depth}", size=11.5, fill=NAVY)

    # Row B: motor driver
    arrow(s, 860, 294, 860, 356, color=BLUE, label="stepper · stepper_ctrl · sstep_motor_*",
          dx=-10, dy=4, anchor="end")
    box(s, 660, 358, 300, 96, "Motor driver: Stepper-C",
        ["sfs,sstep-motor (+ driver, ctrl children)", "slow loop 1 kHz · current loop 20 kHz",
         "trips: overcurrent, VBUS, leg W"],
        fill=SOFT_TEAL, stroke=TEAL, color=TEAL)

    # Row C: power stage backends
    arrow(s, 860, 454, 860, 518, color=TEAL)
    s.text(850, 474, "power_stage API · fast loop in the interrupt", size=12, fill=TEAL,
           anchor="end")
    s.path("M860,498 L500,498 L500,518", color=TEAL, arrow="teal")
    s.text(510, 492, "or, selected by the devicetree", size=11.5, fill=TEAL, anchor="start")
    box(s, 360, 520, 280, 84, "Simulation backend",
        ["sfs,sim-power-stage", "Stepper-Lab plant model (RK4)"],
        fill=WHITE, stroke=TEAL, color=TEAL, dash=True)
    box(s, 660, 520, 300, 84, "STM32 backend",
        ["sfs,stm32-shared-leg-power-stage", "LL: TIM1, ADC1/ADC2, OPAMP1..3"],
        fill=SOFT_TEAL, stroke=TEAL, color=TEAL)

    # Rules
    s.rect(40, 340, 290, 264, fill=LIGHT, stroke=LINE, rx=10)
    s.text(56, 368, "Rules, checked in CI", size=14, weight="bold", fill=NAVY, anchor="start")
    rules = [
        ("1", "Drivers create no threads"),
        ("", "and no queues"),
        ("2", "The servo driver and the motor"),
        ("", "driver never know each other:"),
        ("", "only joint_bridge knows both"),
        ("3", "Only a power_stage backend"),
        ("", "touches registers"),
        ("4", "The devicetree selects"),
        ("", "the hardware"),
    ]
    for i, (num, value) in enumerate(rules):
        y = 396 + i * 22
        if num:
            s.text(64, y, num, size=13, weight="bold", fill=BLUE)
        s.text(82, y, value, size=13, fill=NAVY, anchor="start")

    # Board
    arrow(s, 810, 604, 810, 664, color=ORANGE, label="PWM, ADC samples", dx=10, dy=4,
          anchor="start")
    box(s, 500, 664, 460, 76, "B-G431B-ESC1 and the motor",
        ["L6387 gate drivers → 3 half-bridges U, V, W", "stepper coils A (U–W) and B (V–W)"],
        fill=SOFT_ORANGE, stroke=ORANGE, color=ORANGE)
    box(s, 40, 664, 420, 76, "Faults never wait for a thread",
        ["the interrupt switches the outputs off;", "the host sees Hardware Error Status"],
        fill=SOFT_GREEN, stroke=GREEN, color=GREEN)
    s.save(os.path.join(OUT, "architecture.svg"))


def command_path() -> None:
    """Sequence of a Goal Position write, from app/src/joint_bridge.c, app_msg.c and
    control_task.c."""
    lanes = [
        (90, "Host", "ROS 2 / SDK", GREY),
        (270, "X430 driver", "comm_task", TEAL),
        (470, "joint_bridge", "comm_task", BLUE),
        (690, "control_task", "1 kHz loop", BLUE),
        (890, "Motor driver", "Stepper-C", TEAL),
    ]
    s = Svg(1000, 600, "A Goal Position write, from the bus to the motor and back")
    top, bottom = 30, 570
    for x, name, sub, color in lanes:
        s.rect(x - 80, top, 160, 54, fill=WHITE, stroke=color, rx=10, sw=1.8)
        s.text(x, top + 23, name, size=14.5, weight="bold", fill=color)
        s.text(x, top + 42, sub, size=12, fill=GREY)
        s.line(x, top + 54, x, bottom, color=LINE, dash=True)

    def msg(y, a, b, label, color=NAVY, back=False):
        x1, x2 = lanes[a][0], lanes[b][0]
        s.line(x1, y, x2 + (-6 if x2 > x1 else 6), y, color=color, arrow="end", dash=back)
        s.text((x1 + x2) / 2, y - 7, label, size=12, fill=color)

    def note(y, lane, lines, color):
        x = lanes[lane][0]
        h = 14 + 16 * len(lines)
        s.rect(x - 95, y, 190, h, fill=LIGHT, stroke=color, rx=6)
        for i, value in enumerate(lines):
            s.text(x, y + 19 + i * 16, value, size=11.5, fill=color)

    msg(118, 0, 1, "write Goal Position (116)")
    msg(156, 1, 2, "write callback")
    note(170, 2, ["convert units and profile", "read the latest status"], BLUE)
    msg(242, 2, 3, "app_call(MOVE_TO) → cmd_q")
    msg(282, 3, 4, "stepper_ctrl_configure_ramp()")
    msg(318, 3, 4, "stepper_ctrl_move_to()")
    note(334, 3, ["publish the status → status_q", "then answer"], BLUE)
    msg(406, 3, 2, "result, sequence number ← rsp_q", back=True)
    note(420, 2, ["late answer? discarded", "by its sequence number"], BLUE)
    msg(490, 2, 1, "result (0 or −errno)", back=True)
    msg(530, 1, 0, "status packet: OK or Result Fail", back=True)
    s.text(890, 470, "fast loop keeps running", size=12, fill=TEAL)
    s.text(890, 488, "in the 20 kHz interrupt", size=12, fill=TEAL)
    s.save(os.path.join(OUT, "command-path.svg"))


def test_setup() -> None:
    """SIL and integration tests: pytest drives the native_sim firmware with the plant model
    over Protocol 2.0 (scripts/run-tests.sh, tests/conftest.py)."""
    s = Svg(1000, 300, "SIL and integration tests on the PC")
    box(s, 30, 70, 210, 110, "pytest", ["tests/sil: dxl2.py client", "tests/integration:",
                                       "DynamixelSDK"], fill=SOFT_GREEN, stroke=GREEN,
        color=GREEN)
    arrow(s, 240, 110, 316, 110, color=NAVY, label="Protocol 2.0", dy=-8)
    box(s, 318, 80, 150, 60, "pseudoterminal", ["ttyDXL"], fill=LIGHT, stroke=GREY)
    arrow(s, 468, 110, 536, 110, color=NAVY)
    s.rect(538, 40, 432, 180, fill=SOFT_BLUE, stroke=BLUE, rx=12, sw=1.8)
    s.text(554, 64, "zephyr.exe · native_sim, real time", size=14, weight="bold", fill=BLUE,
           anchor="start")
    chain = [("X430 emulator", TEAL), ("Stepper-C", TEAL), ("sstep_plant.c", ORANGE)]
    for i, (name, color) in enumerate(chain):
        x = 556 + i * 138
        s.rect(x, 86, 122, 48, fill=WHITE, stroke=color, rx=8)
        s.text(x + 61, 115, name, size=13, weight="bold", fill=color)
        if i:
            arrow(s, x - 16, 110, x - 2, 110, color=color)
    s.text(754, 166, "the same application as on the board,", size=12.5, fill=GREY)
    s.text(754, 184, "with the simulation power stage and the motor model", size=12.5,
           fill=GREY)
    s.path("M754,220 L754,250 L135,250 L135,184", color=ORANGE, arrow="orange")
    s.text(444, 270, "model log every 100 ms: rotor position, lag, speed, currents", size=12.5,
           fill=ORANGE)
    s.save(os.path.join(OUT, "test-setup.svg"))


# Motor wiring. Board side: pins of app/boards/b_g431b_esc1.overlay and UM2516 Rev 4,
# Table 4 (boards/st/b_g431b_esc1/doc/index.rst). Motor side: values of the motor node and
# the lead colours of the 17HS19-2004S1 datasheet.
WIRE_BLACK = "#1B1B1B"
WIRE_GREEN = "#2E7D32"
WIRE_RED = "#C62828"
WIRE_BLUE = "#1565C0"
LEGS = (  # name, centre y, PWM pins, current sense
    ("U", 160, "TIM1 CH1 / CH1N: PA8 / PC13", "OPAMP1 → PA2, ADC1_IN3"),
    ("V", 275, "TIM1 CH2 / CH2N: PA9 / PA12", "OPAMP2 → PA6, ADC2_IN3"),
    ("W", 390, "TIM1 CH3 / CH3N: PA10 / PB15", "OPAMP3 → PB1, ADC1_IN12"),
)


def _coil(s: Svg, x, y_top, y_bottom, color) -> None:
    """A winding drawn as four half loops between two terminals at x - 30."""
    loops = 4
    step = (y_bottom - y_top - 20) / loops
    d = f"M{x - 30},{y_top} L{x},{y_top} L{x},{y_top + 10}"
    for _ in range(loops):
        d += f" a{step / 2},{step / 2} 0 0 1 0,{step}"
    d += f" L{x},{y_bottom} L{x - 30},{y_bottom}"
    s.path(d, color=color, width=2.2)


def motor_wiring() -> None:
    """The two coils of the stepper on the three legs of the B-G431B-ESC1 (shared leg W),
    with the MCU pins of every leg, the bus and the supply. Matches docs/hardware.md."""
    s = Svg(1000, 640, "Wiring of the 17HS19-2004S1 stepper motor to the B-G431B-ESC1")

    # Board frame
    s.rect(20, 70, 540, 480, fill=WHITE, stroke=BLUE, rx=14, sw=2)
    s.text(40, 98, "B-G431B-ESC1", size=17, weight="bold", fill=BLUE, anchor="start")
    s.text(540, 98, "pins: UM2516 Rev 4, Table 4", size=12, fill=GREY, anchor="end")

    # MCU and the PWM / current sense lines to the legs
    box(s, 40, 115, 190, 330, "STM32G431CB", ["Cortex-M4F, 170 MHz", "", "TIM1: PWM 20 kHz,",
                                               "center-aligned", "", "OPAMP1..3 + ADC1/2:",
                                               "phase currents", "", "ADC1_IN1: VBUS"],
        fill=SOFT_BLUE, stroke=BLUE, color=BLUE)
    for name, y, pwm, sense in LEGS:
        s.line(230, y, 290, y, color=TEAL, width=2)
        s.rect(290, y - 45, 200, 90, fill=SOFT_TEAL, stroke=TEAL, rx=10)
        s.text(390, y - 22, f"Leg {name}", size=15, weight="bold", fill=TEAL)
        s.text(390, y - 2, pwm, size=11.5, fill=GREY)
        s.text(390, y + 15, "3 mΩ low-side shunt →", size=11.5, fill=GREY)
        s.text(390, y + 32, sense, size=11.5, fill=GREY)
        s.line(490, y, 530, y, color=NAVY, width=2.2)
        s.rect(530, y - 16, 32, 32, fill=NAVY, stroke=NAVY, rx=4)
        s.text(546, y + 6, name, size=16, weight="bold", fill=WHITE)

    # Supply input and debugger
    box(s, 290, 460, 200, 76, "Power input + / −", ["VBUS 10–28 V, nominal 24 V",
                                                       "sense: PA0, 18 kΩ / 169 kΩ"],
        fill=SOFT_ORANGE, stroke=ORANGE, color=ORANGE, size=14)
    s.line(390, 536, 390, 590, color=ORANGE, width=2.2)
    s.text(400, 574, "DC supply with", size=12, fill=ORANGE, anchor="start")
    s.text(400, 590, "a current limit", size=12, fill=ORANGE, anchor="start")
    box(s, 40, 460, 190, 76, "ST-LINK/V2-1", ["USB: VCP = USART2", "PB3 TX / PB4 RX, 1 Mbps",
                                               ], fill=SOFT_GREEN, stroke=GREEN, color=GREEN,
        size=14)
    s.line(135, 536, 135, 590, color=GREEN, width=2.2)
    s.text(145, 574, "USB to the host:", size=12, fill=GREEN, anchor="start")
    s.text(145, 590, "DYNAMIXEL bus, SWD, RTT log", size=12, fill=GREEN, anchor="start")

    # Motor
    s.rect(650, 70, 330, 400, fill=WHITE, stroke=GREY, rx=14, sw=2)
    s.text(815, 98, "17HS19-2004S1 (NEMA 17)", size=16, weight="bold", fill=NAVY)
    s.text(815, 118, "2.0 A · 1.6 Ω · 3.0 mH per coil · 200 steps/rev", size=12, fill=GREY)
    term = {"A+": 160, "A-": 225, "B+": 275, "B-": 340}
    _coil(s, 740, term["A+"], term["A-"], NAVY)
    _coil(s, 740, term["B+"], term["B-"], NAVY)
    s.text(760, 197, "coil A", size=13, weight="bold", fill=NAVY, anchor="start")
    s.text(760, 312, "coil B", size=13, weight="bold", fill=NAVY, anchor="start")
    s.parts.append(f'<circle cx="890" cy="250" r="52" fill="{LIGHT}" stroke="{GREY}" '
                   f'stroke-width="1.6"/>')
    s.text(890, 255, "rotor", size=13, fill=GREY)
    for name, y in term.items():
        s.parts.append(f'<circle cx="710" cy="{y}" r="5" fill="{WHITE}" stroke="{NAVY}" '
                       f'stroke-width="2"/>')
        s.text(700, y - 9, name.replace("-", "−"), size=12, weight="bold", fill=NAVY,
               anchor="end")

    # Leads: U → A+ (black), V → B+ (red), W → A− (green) and B− (blue)
    s.path(f"M562,160 L705,160", color=WIRE_BLACK, width=3)
    s.path(f"M562,275 L594,275 A6,6 0 0 1 606,275 L705,275", color=WIRE_RED, width=3)
    s.path(f"M562,390 L600,390 L600,{term['A-']} L705,{term['A-']}", color=WIRE_GREEN, width=3)
    s.path(f"M600,390 L630,390 L630,{term['B-']} L705,{term['B-']}", color=WIRE_BLUE, width=3)
    s.parts.append(f'<circle cx="600" cy="390" r="5" fill="{NAVY}"/>')
    s.text(615, 152, "black", size=12, fill=WIRE_BLACK, anchor="start")
    s.text(615, 267, "red", size=12, fill=WIRE_RED, anchor="start")
    s.text(640, 217, "green", size=12, fill=WIRE_GREEN, anchor="end")
    s.text(640, 332, "blue", size=12, fill=WIRE_BLUE, anchor="start")

    # Notes
    notes = [
        ("Coil A: U to W. Coil B: V to W.", NAVY, "bold"),
        ("W is the shared leg: it carries iA + iB,", GREY, "normal"),
        ("up to √2 × the phase current.", GREY, "normal"),
        ("Lead colours from the motor datasheet: check", GREY, "normal"),
        ("the pairs with an ohmmeter (≈ 1.6 Ω in a coil,", GREY, "normal"),
        ("open between the coils).", GREY, "normal"),
        ("A reversed coil flips its current sign:", ORANGE, "bold"),
        ("bring-up step 3 checks it.", ORANGE, "bold"),
    ]
    for i, (value, color, weight) in enumerate(notes):
        s.text(660, 490 + i * 18, value, size=12, fill=color, weight=weight, anchor="start")
    s.save(os.path.join(OUT, "motor-wiring.svg"))


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    hero()
    architecture()
    command_path()
    test_setup()
    motor_wiring()
    for name in sorted(os.listdir(OUT)):
        if name.endswith(".svg"):
            print("wrote", os.path.join("docs", "assets", "images", name))


if __name__ == "__main__":
    main()
