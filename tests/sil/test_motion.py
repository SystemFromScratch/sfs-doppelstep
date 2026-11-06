# SPDX-License-Identifier: Apache-2.0
"""Motion through the DYNAMIXEL Control Table, checked on the registers and on
the rotor of the simulated motor (log of the model)."""

import time

import pytest

import dxl2
from dxl2 import DxlError
from support import (CURRENT_UNIT_MA, DXL_ID, START_POSITION, VELOCITY_UNIT_RPM,
                     assert_stopped, pulses_to_usteps, wait_until)

RUN_MA = 1500                     # CONFIG_SFS_CTRL_I_RUN_MA
HOLD_MA = 500                     # CONFIG_SFS_CTRL_I_HOLD_MA


def move_and_wait(bus, goal, timeout=5.0):
    bus.write(DXL_ID, dxl2.GOAL_POSITION, goal)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 1 or
               bus.read(DXL_ID, dxl2.PRESENT_POSITION) == goal, 1.0, what="move start")
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0, timeout, what="move end")


def test_goal_position_moves_the_rotor(fw, bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    since = fw.mark()
    goal = START_POSITION + 1024                     # +90 degrees
    move_and_wait(bus, goal)

    assert bus.read(DXL_ID, dxl2.PRESENT_POSITION) == goal
    assert bus.read(DXL_ID, dxl2.MOVING_STATUS) & 0x01      # in position
    s = fw.next_sample()
    assert s.cmd == pulses_to_usteps(1024)
    assert abs(s.rotor - s.cmd) <= 2, "rotor of the model is not on the commanded position"
    samples = fw.samples(since)
    assert samples and max(abs(x.lag) for x in samples) < 90, "close to losing steps"


def test_run_then_hold_current(fw, bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    move_and_wait(bus, START_POSITION + 512)
    run = bus.read(DXL_ID, dxl2.PRESENT_CURRENT) * CURRENT_UNIT_MA
    assert run == pytest.approx(RUN_MA, abs=40)
    assert fw.next_sample().i_amp == pytest.approx(RUN_MA, abs=40)

    time.sleep(0.8)                                  # CONFIG_SFS_CTRL_HOLD_DELAY_MS = 500
    hold = bus.read(DXL_ID, dxl2.PRESENT_CURRENT) * CURRENT_UNIT_MA
    assert hold == pytest.approx(HOLD_MA, abs=40)
    assert fw.next_sample().i_amp == pytest.approx(HOLD_MA, abs=40)


def test_profile_velocity_sets_the_speed(fw, bus):
    bus.write(DXL_ID, dxl2.PROFILE_VELOCITY, 200)    # 45.8 rev/min
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    # half a revolution: Goal Position stays within Max Position Limit (4095)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 2000)
    peak = wait_until(lambda: (v := bus.read(DXL_ID, dxl2.PRESENT_VELOCITY)) >= 199 and v,
                      2.0, what="cruise speed")
    assert peak <= 201
    rpm = 200 * VELOCITY_UNIT_RPM
    assert fw.next_sample().rpm == pytest.approx(rpm, abs=2)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0, 3.0, what="move end")


def test_time_based_profile_keeps_the_time(fw, bus):
    bus.write(DXL_ID, dxl2.DRIVE_MODE, 4)            # time-based profile (OpenMANIPULATOR-X)
    bus.write(DXL_ID, dxl2.PROFILE_VELOCITY, 1000)   # whole move [ms]
    bus.write(DXL_ID, dxl2.PROFILE_ACCELERATION, 250)  # acceleration time [ms]
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    # Firmware clock: the simulated time of native_sim can run ahead of the host clock
    t0 = bus.read(DXL_ID, dxl2.REALTIME_TICK)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 1024)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 1, 0.5, interval=0.01,
               what="move start")
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0, 3.0, interval=0.01,
               what="move end")
    elapsed_ms = (bus.read(DXL_ID, dxl2.REALTIME_TICK) - t0) % 32768  # 0..32767 ms, then wraps
    assert elapsed_ms / 1000 == pytest.approx(1.0, abs=0.15)
    s = fw.next_sample()
    assert abs(s.rotor - s.cmd) <= 2


def test_velocity_mode(fw, bus):
    bus.write(DXL_ID, dxl2.OPERATING_MODE, 1)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.GOAL_VELOCITY, -100)      # -22.9 rev/min
    wait_until(lambda: bus.read(DXL_ID, dxl2.PRESENT_VELOCITY) == -100, 2.0,
               what="goal velocity")
    time.sleep(0.3)
    s = fw.next_sample()
    assert s.rpm == pytest.approx(-100 * VELOCITY_UNIT_RPM, abs=1.5)
    assert abs(s.lag) < 90

    bus.write(DXL_ID, dxl2.GOAL_VELOCITY, 0)
    wait_until(lambda: bus.read(DXL_ID, dxl2.MOVING) == 0, 2.0, what="stop")
    time.sleep(0.3)
    s = assert_stopped(fw)
    assert abs(s.rotor - s.cmd) <= 2


def test_torque_off_releases_the_motor(fw, bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    move_and_wait(bus, START_POSITION + 256)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 0)
    time.sleep(0.15)
    s = fw.next_sample()
    assert (s.i_a, s.i_b) == (0, 0)
    assert bus.read(DXL_ID, dxl2.PRESENT_CURRENT) == 0


def test_unsupported_operating_mode_is_refused(bus):
    with pytest.raises(DxlError) as err:
        bus.write(DXL_ID, dxl2.OPERATING_MODE, 16)   # PWM control: not supported
    assert err.value.error == dxl2.ERR_DATA_RANGE
    assert bus.read(DXL_ID, dxl2.OPERATING_MODE) == 3


def test_eeprom_is_locked_while_torque_is_on(bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    with pytest.raises(DxlError) as err:
        bus.write(DXL_ID, dxl2.OPERATING_MODE, 1)
    assert err.value.error == dxl2.ERR_ACCESS
