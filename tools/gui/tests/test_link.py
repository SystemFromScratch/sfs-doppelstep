# SPDX-License-Identifier: Apache-2.0
"""SerialLink on the pyserial loopback URL.

On Windows every change of `serial.timeout` makes pyserial call SetCommState: the
USB CDC driver sends SET_LINE_CODING and the ST-LINK Virtual COM Port of the
B-G431B-ESC1 re-initialises its UART, losing the bytes in flight. A status packet
arrives 500 us after the instruction (Return Delay Time 250), right in that window:
most answers were lost. The link must therefore keep the port configuration fixed
after opening it.
"""

import time

from doppelstep_gui.link import SerialLink


def open_loop():
    link = SerialLink("loop://", 1000000)
    calls = []
    original = link._serial._reconfigure_port

    def spy(*args, **kwargs):
        calls.append(1)
        return original(*args, **kwargs)

    link._serial._reconfigure_port = spy
    return link, calls


def test_read_never_reconfigures_the_port():
    link, calls = open_loop()
    link.write(b"\xff\xff\xfd\x00\x01")
    assert link.read(0.2) == b"\xff\xff\xfd\x00\x01"
    assert link.read(0.01) == b""
    assert calls == []
    link.close()


def test_read_returns_early_once_data_arrived():
    link, _ = open_loop()
    link.write(b"abc")
    t0 = time.monotonic()
    assert link.read(1.0) == b"abc"
    assert time.monotonic() - t0 < 0.2
    link.close()


def test_read_without_data_waits_about_the_timeout():
    link, _ = open_loop()
    t0 = time.monotonic()
    assert link.read(0.1) == b""
    waited = time.monotonic() - t0
    assert 0.08 <= waited < 0.3
    link.close()


def test_read_with_zero_timeout_returns_what_is_waiting():
    link, _ = open_loop()
    assert link.read(0) == b""
    link.write(b"xy")
    time.sleep(0.01)
    assert link.read(0) == b"xy"
    link.close()
