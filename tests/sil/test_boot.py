# SPDX-License-Identifier: Apache-2.0
"""Startup: the firmware boots on the simulated motor and answers as an XM430-W350."""

import os
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


def test_model_information_is_the_build_version(exe, bus):
    # cmake/doppelstep_version.cmake writes the version of the build next to zephyr/
    header = os.path.join(os.path.dirname(os.path.dirname(exe)), "doppelstep",
                          "doppelstep_version.h")
    with open(header, encoding="utf-8") as f:
        fields = dict(re.findall(r"#define DOPPELSTEP_VERSION_(\w+)\s+(\d+)U", f.read()))
    expected = (int(fields["PATCH"]) | int(fields["MINOR"]) << 8 | int(fields["MAJOR"]) << 16
                | (min(int(fields["COMMITS"]), 127) | int(fields["DIRTY"]) << 7) << 24)
    assert bus.read(DXL_ID, dxl2.MODEL_INFORMATION) == expected
    assert bus.ping(DXL_ID)[1] == 52                     # Firmware Version stays the XM430's


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
    assert bus.read(DXL_ID, dxl2.BAUD_RATE) == 1                   # 57600 bps, as an XM430
    assert bus.read(DXL_ID, dxl2.OPERATING_MODE) == 3              # position control
    # Factory limits of the board and the motor from the devicetree, not those of an
    # XM430 (doppelstep_config.h, scripts/gen_doppelstep_config.py):
    # 2000 mA rated current / 2.69 mA = 743 (rounded down), 300 rev/min of the motor
    # above the largest X430 Velocity Limit 1023 (234 rev/min), bus voltage trips
    # 10 / 28 V, power stage limit 80 degC
    assert bus.read(DXL_ID, dxl2.CURRENT_LIMIT) == 743
    assert bus.read(DXL_ID, dxl2.VELOCITY_LIMIT) == 1023
    assert bus.read(DXL_ID, dxl2.MAX_VOLTAGE_LIMIT) == 280
    assert bus.read(DXL_ID, dxl2.MIN_VOLTAGE_LIMIT) == 100
    assert bus.read(DXL_ID, dxl2.TEMPERATURE_LIMIT) == 80
