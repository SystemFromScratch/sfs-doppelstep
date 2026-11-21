# SPDX-License-Identifier: Apache-2.0
"""Encoder of the joint on the simulated motor (-sim_encoder): Present Position from the
shaft, the absolute angle at the start, lost steps, and the position regulator at
standstill (app/src/enc_track.h, docs/encoder.md).

The simulated encoder reads the rotor of the model with a resolution of one microstep
(1.28 pulses), so a position check allows one or two pulses.
"""

import re
import time

import dxl2
from support import DXL_ID, START_POSITION, PULSES_PER_REV, USTEPS_PER_REV, wait_until

ENCODER = "-sim_encoder"
HW_OVERLOAD = 0x20
HW_MOTOR_ENCODER = 0x08
POSITION_P_GAIN = dxl2._reg("Position P Gain")
POSITION_TRAJECTORY = dxl2._reg("Position Trajectory")


def usteps_to_pulses(usteps):
    return usteps * PULSES_PER_REV / USTEPS_PER_REV


def settle(bus, target, timeout=3.0, tolerance=2):
    """Wait for the end of the move and for Present Position near the target."""
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0 and
               abs(bus.read(DXL_ID, dxl2.PRESENT_POSITION) - target) <= tolerance,
               timeout, what=f"Present Position near {target}")


def test_start_position_is_the_absolute_shaft_angle(firmware_factory):
    # magnet mounted so that the start angle of the rotor reads a quarter turn
    fw = firmware_factory([ENCODER, "-sim_encoder_mount=16384"])
    bus = dxl2.Bus(fw.pty)
    assert bus.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION + 1024
    # the angle is signed: three quarters read as minus a quarter
    fw2 = firmware_factory([ENCODER, "-sim_encoder_mount=49152"], name="fw2")
    bus2 = dxl2.Bus(fw2.pty)
    assert bus2.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION - 1024


def test_enable_and_move_keep_present_on_the_rotor(firmware_factory):
    fw = firmware_factory([ENCODER, "-sim_encoder_mount=5000"])
    bus = dxl2.Bus(fw.pty)
    start = bus.read(DXL_ID, dxl2.PRESENT_POSITION)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    time.sleep(0.3)                                       # snap and alignment
    goal = start + 1024
    bus.write(DXL_ID, dxl2.GOAL_POSITION, goal)
    settle(bus, goal)
    s = fw.next_sample()
    assert abs(s.rotor - s.cmd) <= 2, "the commanded position sits on the rotor"
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0


def test_present_position_is_the_rotor_and_trajectory_the_command(firmware_factory):
    # 0.08 N*m: at the hold current (0.5 A, after 500 ms at rest) a load angle of about
    # 50 deg el, 9 microsteps, 11 pulses (test_protection.py); regulator off
    fw = firmware_factory([ENCODER, "-sim_load_unm=80000"])
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, POSITION_P_GAIN, 0)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    time.sleep(0.3)
    goal = START_POSITION + 512
    bus.write(DXL_ID, dxl2.GOAL_POSITION, goal)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0, 3.0, what="move end")
    time.sleep(0.8)                                       # hold current
    s = fw.next_sample()
    present = bus.read(DXL_ID, dxl2.PRESENT_POSITION)
    trajectory = bus.read(DXL_ID, POSITION_TRAJECTORY)
    assert abs(usteps_to_pulses(s.cmd - s.rotor)) >= 3, "scenario: the load makes a lag"
    # Model sample and registers a few ms apart, encoder steps of one microstep
    assert abs(present - (START_POSITION + usteps_to_pulses(s.shaft))) <= 2, "Present = shaft"
    assert abs(trajectory - (START_POSITION + usteps_to_pulses(s.cmd))) <= 1, "Trajectory = command"
    # The alignment after the enable took the rotor under the run-current load angle as
    # the field position; at the hold current the load angle is larger: open loop, the
    # difference stays as an error
    assert abs(goal - present) >= 3, "open loop: an error of the load angle remains"


def test_regulator_removes_the_static_error(firmware_factory):
    fw = firmware_factory([ENCODER, "-sim_load_unm=80000"])
    bus = dxl2.Bus(fw.pty)
    assert bus.read(DXL_ID, POSITION_P_GAIN) == 800       # factory value of an XM430
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    time.sleep(0.3)
    goal = START_POSITION + 512
    bus.write(DXL_ID, dxl2.GOAL_POSITION, goal)
    settle(bus, goal, tolerance=1)
    time.sleep(0.8)                                       # also at the hold current
    assert abs(bus.read(DXL_ID, dxl2.PRESENT_POSITION) - goal) <= 1
    assert bus.read(DXL_ID, dxl2.MOVING) == 0, "a correction is no move of the host"
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0


def test_lost_synchronism_is_reported_as_overload(firmware_factory):
    # The scenario of test_protection.py: held at the run current, lost at the hold
    # current. With the encoder the slip is seen, Overload Error switches the torque off.
    fw = firmware_factory([ENCODER, "-sim_load_unm=120000"])
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, POSITION_P_GAIN, 0)                  # the open-loop motor of the test
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 1024)
    wait_until(lambda: bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) & HW_OVERLOAD, 3.0,
               what="Overload Error")
    wait_until(lambda: bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0, 1.0,
               what="torque off (Shutdown mask)")
    assert fw.wait_for(re.compile(r"lost steps")), "the log tells why"

    bus.reboot(DXL_ID)                                      # clears the error (XM430)
    time.sleep(0.2)
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0
