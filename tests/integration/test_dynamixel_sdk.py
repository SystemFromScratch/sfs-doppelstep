# SPDX-License-Identifier: Apache-2.0
"""Integration with the official DYNAMIXEL SDK (Python), the library under the
ROS 2 dynamixel_hardware_interface: same instructions, same timeouts, the
control loop of ros2_control (Sync Read of the present values, Sync Write of
the goals at 100 Hz) against the native_sim firmware and its motor model."""

import math
import time

import pytest
from dynamixel_sdk import COMM_SUCCESS, GroupSyncRead, GroupSyncWrite, PacketHandler, PortHandler

from support import DXL_ID, START_POSITION, pulses_to_usteps

pytestmark = pytest.mark.integration

ADDR_TORQUE_ENABLE = 64
ADDR_GOAL_POSITION = 116
ADDR_PRESENT_CURRENT = 126         # Present Current, Velocity, Position: 10 bytes
LEN_PRESENT = 10


@pytest.fixture
def sdk(fw):
    port = PortHandler(fw.pty)
    assert port.openPort()
    assert port.setBaudRate(1000000)
    yield port, PacketHandler(2.0)
    port.closePort()


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def test_sdk_ping_read_write(sdk):
    port, ph = sdk
    model, result, error = ph.ping(port, DXL_ID)
    assert (result, error, model) == (COMM_SUCCESS, 0, 1020)
    result, error = ph.write1ByteTxRx(port, DXL_ID, ADDR_TORQUE_ENABLE, 1)
    assert (result, error) == (COMM_SUCCESS, 0)
    value, result, error = ph.read1ByteTxRx(port, DXL_ID, ADDR_TORQUE_ENABLE)
    assert (result, error, value) == (COMM_SUCCESS, 0, 1)


def test_ros2_control_loop_tracks_a_trajectory(fw, sdk):
    """100 Hz read / write cycle with a sine goal, as dynamixel_hardware_interface."""
    port, ph = sdk
    assert ph.write1ByteTxRx(port, DXL_ID, ADDR_TORQUE_ENABLE, 1) == (COMM_SUCCESS, 0)
    reader = GroupSyncRead(port, ph, ADDR_PRESENT_CURRENT, LEN_PRESENT)
    assert reader.addParam(DXL_ID)
    writer = GroupSyncWrite(port, ph, ADDR_GOAL_POSITION, 4)

    period, duration, amplitude = 0.01, 3.0, 400        # 100 Hz, 3 s, +-35 degrees
    cycles = failures = 0
    errors = []
    since = fw.mark()
    t0 = time.monotonic()
    next_t = t0
    while (now := time.monotonic()) - t0 < duration:
        t = now - t0
        goal = START_POSITION + round(amplitude * math.sin(2 * math.pi * t / 2.0))

        cycles += 1
        if reader.txRxPacket() != COMM_SUCCESS or not reader.isAvailable(
                DXL_ID, ADDR_PRESENT_CURRENT, LEN_PRESENT):
            failures += 1
        else:
            present = signed(reader.getData(DXL_ID, ADDR_PRESENT_CURRENT + 6, 4), 32)
            if t > 0.5:
                errors.append(abs(present - last_goal))
        writer.clearParam()
        writer.addParam(DXL_ID, list(goal.to_bytes(4, "little", signed=True)))
        assert writer.txPacket() == COMM_SUCCESS
        last_goal = goal

        next_t += period
        time.sleep(max(0.0, next_t - time.monotonic()))

    rate = cycles / (time.monotonic() - t0)
    assert failures == 0, f"{failures} of {cycles} sync reads failed"
    assert rate > 50, f"loop ran at {rate:.0f} Hz"
    # Present Position is the commanded trajectory: within a few cycles of motion
    max_step = amplitude * 2 * math.pi / 2.0 / rate      # pulses per cycle at the peak speed
    assert max(errors) < 4 * max_step, f"tracking error {max(errors)} pulses"

    samples = fw.samples(since)
    assert samples and max(abs(s.lag) for s in samples) < 90, "rotor close to losing steps"
    assert all(abs(s.rotor - s.cmd) < pulses_to_usteps(64) for s in samples)
