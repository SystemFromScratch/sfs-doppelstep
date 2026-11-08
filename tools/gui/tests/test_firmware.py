# SPDX-License-Identifier: Apache-2.0
import pytest

from doppelstep_gui import firmware, image
from doppelstep_gui.device import Bus
from fake_servo import FakeServo
from test_image import make_image


class FakeSmp:
    def __init__(self, answer_after=2, listed_hash=None, version="1.2.3"):
        self.answer_after = answer_after
        self.listed_hash = listed_hash
        self.version = version
        self.calls = []
        self.uploaded = None

    async def open(self):
        self.calls.append("open")

    async def close(self):
        self.calls.append("close")

    async def echo(self, timeout):
        self.calls.append("echo")
        return self.calls.count("echo") >= self.answer_after

    async def images(self):
        self.calls.append("images")
        if self.uploaded is None:
            return []
        return [(0, self.version, self.listed_hash)]

    async def upload(self, data, progress):
        self.calls.append("upload")
        progress(len(data), len(data))
        self.uploaded = data

    async def reset(self):
        self.calls.append("reset")


@pytest.fixture
def img():
    data, _ = make_image()
    return image.parse(data)


def updater(img, smp, servo):
    return firmware.FirmwareUpdater(img, "zephyr.signed.bin", 1,
                                    open_bus=lambda: Bus(servo, timeout=0.05),
                                    smp_session=lambda: smp, log=lambda line: None)


def test_update_passes_when_slot0_holds_the_file(img):
    servo = FakeServo()
    smp = FakeSmp(listed_hash=img.sha256)
    up = updater(img, smp, servo)
    result = up.run(Bus(servo, timeout=0.05))
    assert result.ok, [(s.name, s.detail) for s in result.steps]
    assert servo.reboots == 1 and servo.closed
    assert smp.calls == ["open", "echo", "echo", "images", "upload", "images", "reset", "close"]
    assert smp.uploaded == img.data
    assert up.bus is not None


def test_hash_mismatch_fails_and_does_not_reset(img):
    smp = FakeSmp(listed_hash=b"\x00" * 32)
    result = updater(img, smp, FakeServo()).run(None)
    assert not result.ok
    assert result.steps[-1].name == "Image check"
    assert "reset" not in smp.calls and smp.calls[-1] == "close"


def test_no_valid_image_after_upload_fails(img):
    class Rejecting(FakeSmp):
        async def images(self):
            return []

    result = updater(img, Rejecting(), FakeServo()).run(None)
    assert not result.ok
    assert "no valid image" in result.steps[-1].detail


def test_mcuboot_not_answering(img, monkeypatch):
    monkeypatch.setattr(firmware, "ATTACH_TIMEOUT", 0.05)
    smp = FakeSmp(answer_after=10 ** 9)
    result = updater(img, smp, FakeServo()).run(None)
    assert not result.ok
    assert result.steps[-1].name == "MCUboot serial recovery"
    assert "upload" not in smp.calls


def test_application_must_answer_after_the_reset(img, monkeypatch):
    monkeypatch.setattr(firmware, "APP_START_TIMEOUT", 0.05)
    servo = FakeServo()
    servo.silent = True
    result = updater(img, FakeSmp(listed_hash=img.sha256), servo).run(None)
    assert not result.ok
    assert result.steps[-1].name == "Application"


def test_image_too_large_stops_before_the_reboot():
    data, _ = make_image(body=b"\x00" * (image.SLOT0_SIZE + 1))
    servo = FakeServo()
    result = updater(image.parse(data), FakeSmp(), servo).run(Bus(servo, timeout=0.05))
    assert not result.ok and servo.reboots == 0
