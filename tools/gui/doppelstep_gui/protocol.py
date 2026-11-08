# SPDX-License-Identifier: Apache-2.0
"""DYNAMIXEL Protocol 2.0 packets: CRC, byte stuffing, instruction and status packets.

No I/O and only the standard library: shared by the GUI (device.py) and by the
protocol client of the SIL tests (tools/sim/dxl2.py).
"""

import struct

HEADER = b"\xff\xff\xfd\x00"

INST_PING = 0x01
INST_READ = 0x02
INST_WRITE = 0x03
INST_FACTORY_RESET = 0x06
INST_REBOOT = 0x08
INST_CLEAR = 0x10
INST_STATUS = 0x55
INST_SYNC_READ = 0x82
INST_SYNC_WRITE = 0x83

BROADCAST_ID = 0xFE

# Error numbers of the status packet (bit 7 = Alert: Hardware Error Status != 0)
ERR_RESULT_FAIL = 1
ERR_INSTRUCTION = 2
ERR_CRC = 3
ERR_DATA_RANGE = 4
ERR_DATA_LENGTH = 5
ERR_DATA_LIMIT = 6
ERR_ACCESS = 7
ALERT = 0x80

ERROR_NAMES = {
    ERR_RESULT_FAIL: "Result Fail",
    ERR_INSTRUCTION: "Instruction Error",
    ERR_CRC: "CRC Error",
    ERR_DATA_RANGE: "Data Range Error",
    ERR_DATA_LENGTH: "Data Length Error",
    ERR_DATA_LIMIT: "Data Limit Error",
    ERR_ACCESS: "Access Error",
}

# Parameters of the Clear instruction that clear the multi-turn position
CLEAR_MULTI_TURN = bytes([0x01, 0x44, 0x58, 0x4C, 0x22])


def crc16(data):
    """CRC-16 of Protocol 2.0 (polynomial 0x8005, initial value 0)."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def stuff(params):
    """Byte stuffing: 0xFD after every 0xFF 0xFF 0xFD."""
    out = bytearray()
    for byte in params:
        out.append(byte)
        if out[-3:] == b"\xff\xff\xfd":
            out.append(0xFD)
    return bytes(out)


def unstuff(params):
    """Remove the 0xFD that byte stuffing added after 0xFF 0xFF 0xFD."""
    out = bytearray()
    i = 0
    while i < len(params):
        out.append(params[i])
        if out[-3:] == b"\xff\xff\xfd" and params[i + 1:i + 2] == b"\xfd":
            i += 1
        i += 1
    return bytes(out)


def packet(dxl_id, inst, params=b""):
    """Instruction packet with header, length and CRC."""
    body = stuff(params)
    data = HEADER + bytes([dxl_id]) + struct.pack("<H", len(body) + 3)
    data += bytes([inst]) + body
    return data + struct.pack("<H", crc16(data))


def error_text(error):
    """Readable name of the error byte of a status packet."""
    number = error & 0x7F
    text = ERROR_NAMES.get(number, f"error {number}") if number else "no error"
    return text + (" (Alert)" if error & ALERT else "")


class DxlError(Exception):
    """Status packet with an error number."""

    def __init__(self, dxl_id, error):
        super().__init__(f"ID {dxl_id}: {error_text(error)} (0x{error:02x})")
        self.dxl_id = dxl_id
        self.error = error & 0x7F
        self.alert = bool(error & ALERT)


class Status:
    """One status packet (parameters without byte stuffing)."""

    def __init__(self, raw):
        self.raw = bytes(raw)
        self.id = raw[4]
        self.error = raw[8]
        self.params = unstuff(raw[9:-2])

    @property
    def alert(self):
        return bool(self.error & ALERT)


def take_status(rx):
    """Remove the first complete status packet from the bytearray `rx` and return it.

    Bytes before the header are dropped. Returns None while no complete packet
    is buffered. Raises IOError on a wrong CRC or a packet that is not a status.
    """
    start = rx.find(HEADER)
    if start < 0:
        # keep a possible partial header at the end
        del rx[:max(0, len(rx) - 3)]
        return None
    if len(rx) < start + 7:
        return None
    length = struct.unpack_from("<H", rx, start + 5)[0]
    end = start + 7 + length
    if len(rx) < end:
        return None
    raw = bytes(rx[start:end])
    del rx[:end]
    if length < 4 or struct.unpack_from("<H", raw, len(raw) - 2)[0] != crc16(raw[:-2]):
        raise IOError("status packet with a wrong CRC")
    if raw[7] != INST_STATUS:
        raise IOError(f"not a status packet: instruction 0x{raw[7]:02x}")
    return Status(raw)
