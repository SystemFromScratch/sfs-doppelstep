# SPDX-License-Identifier: Apache-2.0
from doppelstep_gui import registers as r


def test_registers_do_not_overlap_and_are_sorted():
    end = 0
    for reg in r.TABLE:
        assert reg.address >= end, reg.name
        assert reg.size in (1, 2, 4)
        end = reg.address + reg.size
    assert end == 148


def test_eeprom_ends_before_torque_enable():
    assert all((reg.area == "eeprom") == (reg.address < 64) for reg in r.TABLE)


def test_signed_decode_and_encode():
    assert r.PRESENT_CURRENT.decode(b"\xff\xff") == -1
    assert r.GOAL_POSITION.encode(-2) == b"\xfe\xff\xff\xff"
    assert r.BUS_WATCHDOG.decode(b"\xff") == -1
    assert r.MOVING.decode(b"\xff") == 255


def test_scaled_values():
    assert r.PRESENT_INPUT_VOLTAGE.scaled(240) == "24 V"
    assert r.PRESENT_POSITION.scaled(1024) == "90.0°"
    assert r.ID.scaled(7) == "7"


def test_hw_error_text():
    assert r.hw_error_text(0) == "none"
    assert r.hw_error_text(0x11) == "Input Voltage Error, Electrical Shock Error"
    assert r.hw_error_text(0x04) == "bits 0x04"
