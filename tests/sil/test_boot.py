# SPDX-License-Identifier: Apache-2.0
"""Startup: the firmware boots on the simulated motor and answers as an XM430-W350."""

import re

import dxl2
from support import DXL_ID, START_POSITION


def test_startup_log_has_no_error(fw):
    fw.wait_for(re.compile(r"sim_plant: model 17HS19-2004S1: .*leg error on"), 1)
    assert fw.errors() == []


def test_ping_answers_as_xm430_w350(bus):
    model, firmware = bus.ping(DXL_ID)
    assert model == 1020                          # XM430-W350
    assert firmware > 0


def test_live_registers_at_rest(bus):
    assert bus.read(DXL_ID, dxl2.PRESENT_INPUT_VOLTAGE) == 240     # 24.0 V of the model
    assert bus.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION
    assert bus.read(DXL_ID, dxl2.PRESENT_CURRENT) == 0
    assert bus.read(DXL_ID, dxl2.PRESENT_VELOCITY) == 0
    assert bus.read(DXL_ID, dxl2.MOVING) == 0
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0
    assert bus.read(DXL_ID, dxl2.HARDWARE_ERROR_STATUS) == 0
    assert bus.read(DXL_ID, dxl2.PRESENT_TEMPERATURE) == 25        # native_sim: fixed 25.0 degC


def test_factory_eeprom(bus):
    assert bus.read(DXL_ID, dxl2.ID) == 1
    assert bus.read(DXL_ID, dxl2.BAUD_RATE) == 3                   # 1 Mbps
    assert bus.read(DXL_ID, dxl2.OPERATING_MODE) == 3              # position control
    # Factory limits of the real motor, not those of an XM430 (joint_bridge.c):
    # 1500 mA run current / 2.69 mA = 557 (rounded down), 300 rev/min of the motor
    # above the largest X430 Velocity Limit 1023 (234 rev/min)
    assert bus.read(DXL_ID, dxl2.CURRENT_LIMIT) == 557
    assert bus.read(DXL_ID, dxl2.VELOCITY_LIMIT) == 1023
