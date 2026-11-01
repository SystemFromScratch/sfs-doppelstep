# SPDX-License-Identifier: Apache-2.0
"""DYNAMIXEL bus behaviour: bad packets, Bus Watchdog, Reboot, Factory Reset,
Sync Read / Write."""

import time

import pytest

import dxl2
from dxl2 import DxlError
from support import DXL_ID, START_POSITION, wait_until


def test_bad_crc_and_other_ids_get_no_answer(bus):
    bad = bytearray(dxl2.packet(DXL_ID, dxl2.INST_PING))
    bad[-1] ^= 0xFF
    bus.write_raw(bytes(bad))
    assert bus.receive(0.3) is None
    bus.write_raw(dxl2.packet(9, dxl2.INST_PING))
    assert bus.receive(0.3) is None
    assert bus.ping(DXL_ID)[0] == 1020                     # the bus still works


def test_bus_watchdog_stops_the_motor(fw, bus):
    bus.write(DXL_ID, dxl2.OPERATING_MODE, 1)               # velocity control
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.BUS_WATCHDOG, 25)                # 500 ms
    bus.write(DXL_ID, dxl2.GOAL_VELOCITY, 100)
    time.sleep(0.15)
    assert fw.next_sample().rpm != 0

    time.sleep(1.0)                                         # host silent
    assert fw.next_sample().rpm == 0
    assert bus.read(DXL_ID, dxl2.BUS_WATCHDOG) == -1
    with pytest.raises(DxlError) as err:                    # goals refused until cleared
        bus.write(DXL_ID, dxl2.GOAL_VELOCITY, 50)
    assert err.value.error == dxl2.ERR_DATA_RANGE
    bus.write(DXL_ID, dxl2.BUS_WATCHDOG, 0)
    bus.write(DXL_ID, dxl2.GOAL_VELOCITY, 50)


def test_reboot_disables_the_torque_and_keeps_the_position(bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.write(DXL_ID, dxl2.GOAL_POSITION, START_POSITION + 512)
    wait_until(lambda: bus.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION + 512, 3.0,
               what="move end")
    bus.reboot(DXL_ID)
    time.sleep(0.3)
    assert bus.read(DXL_ID, dxl2.TORQUE_ENABLE) == 0
    assert bus.read(DXL_ID, dxl2.PRESENT_POSITION) == START_POSITION + 512


def test_factory_reset_restores_the_id(bus):
    bus.write(DXL_ID, dxl2.ID, 5)
    assert bus.ping(5)[0] == 1020
    bus.factory_reset(5, 0xFF)
    time.sleep(0.3)
    assert bus.ping(DXL_ID)[0] == 1020
    with pytest.raises(TimeoutError):
        bus.ping(5, timeout=0.2)


def test_sync_write_and_sync_read(bus):
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    bus.sync_write(dxl2.GOAL_POSITION, {DXL_ID: START_POSITION - 512})
    wait_until(lambda: bus.sync_read([DXL_ID], dxl2.PRESENT_POSITION)[DXL_ID] ==
               START_POSITION - 512, 3.0, what="sync read of the goal")
