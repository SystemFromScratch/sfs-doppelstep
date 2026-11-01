# SPDX-License-Identifier: Apache-2.0
"""Fixtures of the SIL and integration tests (see tests/README.md).

Every test starts its own native_sim firmware (zephyr.exe built with
apps/doppelstep/sim-test.conf) in a fresh working directory, so the emulated
EEPROM (flash.bin) starts at the factory values. The firmware runs in real
time: the bus timing is the one a host program sees.
"""

import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "sim"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dxl2 import Bus  # noqa: E402
from firmware import NativeFirmware  # noqa: E402


def pytest_addoption(parser):
    parser.addoption("--exe", default=os.environ.get(
        "DOPPELSTEP_EXE", os.path.join(ROOT, "build-tests", "zephyr", "zephyr.exe")),
        help="native_sim firmware (default build-tests/zephyr/zephyr.exe, env DOPPELSTEP_EXE)")


@pytest.fixture(scope="session")
def exe(request):
    path = os.path.abspath(request.config.getoption("--exe"))
    if not os.access(path, os.X_OK):
        pytest.exit(f"{path} not found: build it with scripts/run-tests.sh (or --exe)", 2)
    return path


@pytest.fixture
def firmware_factory(exe, tmp_path, request):
    """start(args=(), name="fw") -> NativeFirmware; every one is stopped at the end."""
    started = []

    def start(args=(), name="fw"):
        fw = NativeFirmware(exe, str(tmp_path / name), args)
        started.append(fw)
        return fw.start()

    yield start
    for fw in started:
        fw.stop()
        log = tmp_path / f"{os.path.basename(fw.workdir)}.log"
        log.write_text("\n".join(fw.lines) + "\n")
    report = getattr(request.node, "rep_call", None)
    if report is not None and report.failed:
        for fw in started:
            print(f"--- firmware output ({fw.workdir}), last lines ---")
            print("\n".join(fw.lines[-40:]))


@pytest.fixture
def fw(firmware_factory):
    """Firmware with the default scenario (no load, nominal bus voltage)."""
    return firmware_factory()


@pytest.fixture
def bus(fw):
    b = Bus(fw.pty)
    yield b
    b.close()


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    report = yield
    setattr(item, "rep_" + report.when, report)
    return report

