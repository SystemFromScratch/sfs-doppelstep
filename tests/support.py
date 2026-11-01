# SPDX-License-Identifier: Apache-2.0
"""Constants and helpers shared by the tests (scaling of the emulated XM430-W350)."""

import time

DXL_ID = 1
START_POSITION = 2048            # Present Position at power-on (CONFIG_SFS_DXL_START_POSITION)
PULSES_PER_REV = 4096
USTEPS_PER_REV = 200 * 16        # 17HS19-2004S1, CONFIG_SFS_CTRL_MICROSTEPS
CURRENT_UNIT_MA = 2.69           # Present Current [2.69 mA]
VELOCITY_UNIT_RPM = 0.229        # Present Velocity [0.229 rev/min]


def pulses_to_usteps(pulses):
    return pulses * USTEPS_PER_REV / PULSES_PER_REV


def wait_until(predicate, timeout, interval=0.05, what="condition"):
    """Poll `predicate` until it returns a truthy value; return that value."""
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() > deadline:
            raise AssertionError(f"timeout after {timeout} s waiting for {what}")
        time.sleep(interval)
