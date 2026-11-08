# SPDX-License-Identifier: Apache-2.0
import pytest

from doppelstep_gui import protocol as p


def test_ping_packet_matches_the_e_manual_example():
    # ROBOTIS e-Manual, Protocol 2.0, Ping example: ID 1
    assert p.packet(1, p.INST_PING) == bytes.fromhex("FFFFFD000103000119 4E".replace(" ", ""))


def test_stuffing_round_trip():
    params = bytes([0xFF, 0xFF, 0xFD, 0x01, 0xFF, 0xFF, 0xFD])
    stuffed = p.stuff(params)
    assert stuffed == bytes([0xFF, 0xFF, 0xFD, 0xFD, 0x01, 0xFF, 0xFF, 0xFD, 0xFD])
    assert p.unstuff(stuffed) == params


def status(dxl_id, error, params):
    return p.packet(dxl_id, p.INST_STATUS, bytes([error]) + params)


def test_take_status_skips_noise_and_waits_for_a_whole_packet():
    raw = status(1, 0, b"\x01\x02")
    rx = bytearray(b"\x00\x13" + raw[:5])
    assert p.take_status(rx) is None
    rx += raw[5:] + b"\xaa"
    s = p.take_status(rx)
    assert (s.id, s.error, s.params) == (1, 0, b"\x01\x02")
    assert rx == bytearray(b"\xaa")


def test_take_status_removes_stuffing_from_the_parameters():
    s = p.take_status(bytearray(status(1, 0, b"\xff\xff\xfd\x07")))
    assert s.params == b"\xff\xff\xfd\x07"


def test_take_status_rejects_a_wrong_crc():
    raw = bytearray(status(1, 0, b""))
    raw[-1] ^= 0xFF
    with pytest.raises(IOError):
        p.take_status(raw)


def test_error_text_and_alert():
    exc = p.DxlError(1, p.ALERT | p.ERR_ACCESS)
    assert exc.error == p.ERR_ACCESS and exc.alert
    assert "Access Error" in str(exc) and "Alert" in str(exc)
