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

KT = 0.2086                       # N*m/A, Stepper-Lab params/motor_17hs19_2004s1.json
RUN_A = 1.5
HOLD_A = 0.5
HW_INPUT_VOLTAGE = 0x01           # Hardware Error Status bits (XM430 e-Manual)
HW_ELECTRICAL_SHOCK = 0x10
HW_OVERHEATING = 0x04


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

    # The model log gives the lag in whole microsteps (5.6 deg el each), and the rotor
    # vibrates by a fraction of a microstep at rest: a single sample can be one microstep
    # off. Average the samples of a window, in the time of the firmware, not of the host
    # (a loaded host runs native_sim slower than real time).
    def mean_lag(t_from, t_to):
        lags = []
        while (s := fw.next_sample()).t < t_to:
            if s.t >= t_from:
                lags.append(s.lag)
        return sum(lags) / len(lags), s

    at_rest = fw.next_sample().t                            # run current, at rest
    lag, _ = mean_lag(at_rest, at_rest + 0.35)
    assert lag == pytest.approx(load_angle(torque, RUN_A), abs=4)
    lag, s = mean_lag(at_rest + 0.8, at_rest + 1.3)         # hold current 500 ms after
    assert lag == pytest.approx(load_angle(torque, HOLD_A), abs=4)
    assert s.cmd == pulses_to_usteps(1024)
    assert abs(s.rotor - s.cmd) < 16, "steps lost"          # 16 microsteps = 1 full step


@pytest.mark.xfail(strict=True, reason="no stall detection: a stepper without encoder loses "
                                       "synchronism silently (docs/overview.md, Status and known limits)")
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


def test_torque_enable_below_vbus_min_is_refused(firmware_factory):
    """Bus voltage outside vbus-min-mv..vbus-max-mv: Torque Enable answers Result Fail
    before anything is energised, Input Voltage Error tells the host why. Nothing is
    latched in the motor: with the supply back the torque comes on."""
    fw = firmware_factory(["-sim_vbus_mv=9000"])           # below vbus-min-mv of the motor node
    bus = dxl2.Bus(fw.pty)
    assert bus.read(DXL_ID, dxl2.PRESENT_INPUT_VOLTAGE) == 90
    with pytest.raises(dxl2.DxlError) as error:
        bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    assert error.value.error == dxl2.ERR_RESULT_FAIL
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) & HW_INPUT_VOLTAGE
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0
    s = fw.next_sample()
    assert (s.i_a, s.i_b) == (0, 0), "outputs must stay off"


def test_torque_enable_in_a_fault_is_rejected(firmware_factory):
    fw = firmware_factory(["-sim_load_unm=400000"])        # overload trip, latched
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    wait_until(lambda: bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0, 2.0, what="trip")
    with pytest.raises(dxl2.DxlError) as error:
        bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    assert error.value.error == dxl2.ERR_RESULT_FAIL
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0


def test_overheating_trips_like_an_xm430(bus):
    """Present Temperature above Temperature Limit raises Overheating Error; it is in the
    factory Shutdown mask, so the torque stays off until a Reboot. native_sim reports a
    fixed 25 degC: a Temperature Limit of 20 degC takes the same path as a hot stage."""
    assert bus.read(DXL_ID, dxl2.PRESENT_TEMPERATURE) == 25
    bus.write(DXL_ID, dxl2.TEMPERATURE_LIMIT, 20)                  # EEPROM, torque off
    wait_until(lambda: bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) & HW_OVERHEATING, 1.0,
               what="Overheating Error")
    with pytest.raises(dxl2.DxlError):
        bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0

    bus.write(DXL_ID, dxl2.TEMPERATURE_LIMIT, 80)                  # status carries the Alert bit only
    bus.reboot(DXL_ID)                                             # clears Hardware Error Status
    wait_until(lambda: bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0, 1.0,
               what="error cleared by the Reboot")
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)                       # works again
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 1
