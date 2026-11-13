# SPDX-License-Identifier: Apache-2.0
import pytest

from doppelstep_gui import poller
from doppelstep_gui import protocol as p
from doppelstep_gui import registers as r
from doppelstep_gui.device import Bus
from fake_servo import FakeServo


@pytest.fixture
def servo():
    return FakeServo()


@pytest.fixture
def bus(servo):
    return Bus(servo, timeout=0.05)


def test_ping_read_write(bus):
    assert bus.ping(1) == (1020, 1)
    bus.write(1, r.GOAL_POSITION, -5)
    assert bus.read(1, r.GOAL_POSITION) == -5


def test_status_error_raises(bus):
    with pytest.raises(p.DxlError) as err:
        bus.write(1, r.OPERATING_MODE, 16)
    assert err.value.error == p.ERR_DATA_RANGE


def test_no_answer_times_out(bus):
    with pytest.raises(TimeoutError):
        bus.ping(9)


def test_answer_waiting_after_the_deadline_is_not_lost(servo):
    """The poller thread can be held past its 0.1 s deadline by the GUI thread (the GIL
    while matplotlib redraws). An answer that arrived meanwhile is in the port: it must be
    read once more before giving up, not reported as no answer."""
    late = Bus(servo, timeout=0.0)          # the deadline has passed before the first read
    assert late.ping(1) == (1020, 1)
    assert late.read(1, r.GOAL_POSITION) == 0


def test_sync_read_and_write(bus):
    bus.sync_write(r.GOAL_POSITION, {1: 1536})
    assert bus.sync_read([1], r.GOAL_POSITION) == {1: 1536}


def test_wait_for_ping_gives_up(bus, servo):
    servo.silent = True
    with pytest.raises(TimeoutError):
        bus.wait_for_ping(1, 0.1, interval=0.01)


def test_live_block_decodes_the_live_registers(bus):
    data = bus.read_block(1, poller.BLOCK_ADDRESS, poller.BLOCK_SIZE)
    values = poller.decode_block(data)
    assert values["Present Position"] == 2048
    sample = poller.Sample(0.5, values)
    assert sample.physical()["present_position_deg"] == pytest.approx(180.0)
    assert "Present Temperature" in values and "temperature_c" in sample.physical()


def test_csv_recorder(tmp_path, bus):
    rec = poller.CsvRecorder(tmp_path / "live.csv")
    rec.add(poller.Sample(0.1, poller.decode_block(
        bus.read_block(1, poller.BLOCK_ADDRESS, poller.BLOCK_SIZE))))
    rec.close()
    lines = (tmp_path / "live.csv").read_text().splitlines()
    assert lines[0].startswith("time_s,goal_position_deg")
    assert len(lines) == 2
