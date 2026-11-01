# SPDX-License-Identifier: Apache-2.0
"""EEPROM area in flash (NVS, flash.bin of native_sim) across a power cycle."""

import dxl2
from support import DXL_ID


def test_eeprom_survives_a_restart_ram_does_not(fw):
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.DRIVE_MODE, 4)                   # EEPROM
    bus.write(DXL_ID, dxl2.ID, 7)                           # EEPROM, last: the ID changes
    bus.write(7, dxl2.PROFILE_VELOCITY, 123)                # RAM
    bus.close()

    fw.restart()                                            # same working directory
    bus = dxl2.Bus(fw.pty)
    assert bus.ping(7)[0] == 1020
    assert bus.read(7, dxl2.DRIVE_MODE) == 4
    assert bus.read(7, dxl2.PROFILE_VELOCITY) == 0
    bus.close()
