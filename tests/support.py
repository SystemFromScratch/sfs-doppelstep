# SPDX-License-Identifier: Apache-2.0
"""Constants and helpers shared by the tests (scaling of the emulated XM430-W350)."""

import time

DXL_ID = 1
START_POSITION = 2048            # Present Position at power-on (start-position of the servo node)
PULSES_PER_REV = 4096
USTEPS_PER_REV = 200 * 16        # 17HS19-2004S1, microsteps of the motor node
CURRENT_UNIT_MA = 2.69           # Present Current [2.69 mA]
VELOCITY_UNIT_RPM = 0.229        # Present Velocity [0.229 rev/min]

# The rotor of the model is never exactly still: one ADC step in the current loop
# excites its mechanical resonance, up to about 3 rpm at rest depending on the angle.
STOPPED_RPM = 5


def pulses_to_usteps(pulses):
    return pulses * USTEPS_PER_REV / PULSES_PER_REV


def assert_stopped(fw):
    """The command stands still and the rotor only vibrates; return the last model sample."""
    first = fw.next_sample()
    second = fw.next_sample()
    assert second.cmd == first.cmd, "the commanded position still moves"
    assert abs(first.rpm) < STOPPED_RPM and abs(second.rpm) < STOPPED_RPM
    return second


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
