# SPDX-License-Identifier: Apache-2.0
"""Load torque and supply faults of the simulated motor, as the host sees them.

The scenario comes from the command line of zephyr.exe (-sim_load_unm,
-sim_vbus_mv). The load is a constant active torque (gravity on a joint)
acting from the first Torque Enable.
"""

import math
import time

import pytest

import dxl2
from support import DXL_ID, START_POSITION, pulses_to_usteps, wait_until

KT = 0.2086                       # N*m/A, sfs-stepper-sim params/motor_17hs19_2004s1.json
RUN_A = 1.5
HOLD_A = 0.5
HW_INPUT_VOLTAGE = 0x01           # Hardware Error Status bits (XM430 e-Manual)
HW_ELECTRICAL_SHOCK = 0x10


def load_angle(torque, current):
    """Electrical load angle of a synchronous stepper [deg]: T = Kt I sin(angle)."""
    return math.degrees(math.asin(torque / (KT * current)))


def test_load_angle_follows_the_torque_equation(firmware_factory):
    torque = 0.08
    fw = firmware_factory([f"-sim_load_unm={int(torque * 1e6)}"])
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 1024)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0 and
               bus.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION + 1024, 3.0,
               what="move end")

    s = fw.next_sample()                                    # run current, at rest
    assert s.lag == pytest.approx(load_angle(torque, RUN_A), abs=4)
    time.sleep(0.8)                                         # hold current after 500 ms
    s = fw.next_sample()
    assert s.lag == pytest.approx(load_angle(torque, HOLD_A), abs=4)
    assert s.cmd == pulses_to_usteps(1024)
    assert abs(s.rotor - s.cmd) < 16, "steps lost"          # 16 microsteps = 1 full step


@pytest.mark.xfail(strict=True, reason="no stall detection: a stepper without encoder loses "
                                       "synchronism silently (README, Open items)")
def test_lost_synchronism_is_reported(firmware_factory):
    # 0.12 N*m: held at the run current (Kt * 1.5 A = 0.31 N*m), lost at the
    # hold current (Kt * 0.5 A = 0.104 N*m): the joint falls.
    fw = firmware_factory(["-sim_load_unm=120000"])
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 1024)
    time.sleep(1.5)
    s = fw.next_sample()
    assert abs(s.rotor - s.cmd) > 64, "scenario: the model must have lost steps"
    reported = (bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) != 0 or
                bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0)
    assert reported, "the host still sees the joint in position"


def test_overload_trips_and_is_reported(firmware_factory):
    fw = firmware_factory(["-sim_load_unm=400000"])        # above Kt * 1.5 A
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    wait_until(lambda: bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0, 2.0,
               what="torque off after the trip")
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) & HW_ELECTRICAL_SHOCK
    assert bus.last_status.alert, "status packets must carry the Alert bit"
    s = fw.next_sample()
    assert (s.i_a, s.i_b) == (0, 0), "outputs must be off"

    bus.reboot(DXL_ID)                                      # clears the error (XM430)
    time.sleep(0.2)
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0
    assert not bus.last_status.alert


def test_undervoltage_is_reported(firmware_factory):
    fw = firmware_factory(["-sim_vbus_mv=9000"])           # below CONFIG_SFS_STAGE_VBUS_MIN_MV
    bus = dxl2.Bus(fw.pty)
    assert bus.read(DXL_ID, dxl2.PRESENT_INPUT_VOLTAGE) == 90
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    wait_until(lambda: bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) & HW_INPUT_VOLTAGE, 1.0,
               what="input voltage error")
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0
