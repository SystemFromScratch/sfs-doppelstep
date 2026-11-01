# SPDX-License-Identifier: Apache-2.0
"""Minimal DYNAMIXEL Protocol 2.0 client (standard library only).

Used by dxl_smoke_test.py and the SIL tests (tests/sil). It speaks to a
pseudoterminal or serial port (Linux) or to the TCP bridge of run_native.py.
Unlike DynamixelSDK it exposes the raw status (error byte, alert bit) and lets
a test send malformed packets.
"""

import os
import select
import socket
import struct
import time

# Control Table of the XM430-W350 (e-Manual): (address, size)
MODEL_NUMBER = (0, 2)
FIRMWARE_VERSION = (6, 1)
ID = (7, 1)
BAUD_RATE = (8, 1)
RETURN_DELAY_TIME = (9, 1)
DRIVE_MODE = (10, 1)
OPERATING_MODE = (11, 1)
HOMING_OFFSET = (20, 4)
CURRENT_LIMIT = (38, 2)
VELOCITY_LIMIT = (44, 4)
TORQUE_ENABLE = (64, 1)
HARDWARE_ERROR_STATUS = (70, 1)
BUS_WATCHDOG = (98, 1)
GOAL_VELOCITY = (104, 4)
PROFILE_ACCELERATION = (108, 4)
PROFILE_VELOCITY = (112, 4)
GOAL_POSITION = (116, 4)
MOVING = (122, 1)
MOVING_STATUS = (123, 1)
PRESENT_CURRENT = (126, 2)
PRESENT_VELOCITY = (128, 4)
PRESENT_POSITION = (132, 4)
PRESENT_INPUT_VOLTAGE = (144, 2)

SIGNED = {HOMING_OFFSET, GOAL_VELOCITY, GOAL_POSITION, PRESENT_CURRENT, PRESENT_VELOCITY,
          PRESENT_POSITION, BUS_WATCHDOG}

INST_PING = 0x01
INST_READ = 0x02
INST_WRITE = 0x03
INST_FACTORY_RESET = 0x06
INST_REBOOT = 0x08
INST_SYNC_READ = 0x82
INST_SYNC_WRITE = 0x83
INST_STATUS = 0x55

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


def packet(dxl_id, inst, params=b""):
    """Instruction packet with header, length and CRC."""
    body = stuff(params)
    data = bytes([0xFF, 0xFF, 0xFD, 0x00, dxl_id]) + struct.pack("<H", len(body) + 3)
    data += bytes([inst]) + body
    return data + struct.pack("<H", crc16(data))


class DxlError(Exception):
    """Status packet with an error number."""

    def __init__(self, dxl_id, error):
        super().__init__(f"ID {dxl_id}: error 0x{error:02x}")
        self.error = error & 0x7F
        self.alert = bool(error & ALERT)


class Status:
    """One status packet."""

    def __init__(self, raw):
        self.id = raw[4]
        self.error = raw[8]
        self.params = raw[9:-2]

    @property
    def alert(self):
        return bool(self.error & ALERT)


class Bus:
    """Protocol 2.0 master on a PTY / serial port (Linux) or a TCP bridge."""

    def __init__(self, port=None, tcp=None):
        if tcp:
            host, tcp_port = tcp.rsplit(":", 1)
            self.sock = socket.create_connection((host, int(tcp_port)))
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.fd = None
        else:
            import tty  # Linux only

            self.sock = None
            self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(self.fd)
        self.rx = bytearray()
        self.last_status = None

    def close(self):
        if self.sock:
            self.sock.close()
        elif self.fd is not None:
            os.close(self.fd)
            self.fd = None

    # --- transport -----------------------------------------------------------

    def write_raw(self, data):
        if self.sock:
            self.sock.sendall(data)
        else:
            os.write(self.fd, data)

    def _read(self, timeout):
        src = self.sock or self.fd
        ready, _, _ = select.select([src], [], [], timeout)
        if not ready:
            return b""
        return self.sock.recv(4096) if self.sock else os.read(self.fd, 4096)

    def receive(self, timeout=0.5):
        """Next status packet, or None after `timeout` seconds."""
        deadline = time.monotonic() + timeout
        while True:
            start = self.rx.find(b"\xff\xff\xfd\x00")
            if start >= 0 and len(self.rx) >= start + 7:
                length = struct.unpack_from("<H", self.rx, start + 5)[0]
                end = start + 7 + length
                if len(self.rx) >= end:
                    raw = bytes(self.rx[start:end])
                    del self.rx[:end]
                    if struct.unpack_from("<H", raw, len(raw) - 2)[0] != crc16(raw[:-2]):
                        raise IOError("status packet with a wrong CRC")
                    if raw[7] != INST_STATUS:
                        raise IOError(f"not a status packet: instruction 0x{raw[7]:02x}")
                    return Status(raw)
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.rx += self._read(left)

    def flush(self):
        self.rx.clear()
        while self._read(0.0):
            pass

    def transact(self, dxl_id, inst, params=b"", timeout=0.5, check=True):
        """Send one instruction and return its status (raises on timeout / error)."""
        self.flush()
        self.write_raw(packet(dxl_id, inst, params))
        status = self.receive(timeout)
        if status is None:
            raise TimeoutError(f"no answer from ID {dxl_id}")
        self.last_status = status
        if check and (status.error & 0x7F):
            raise DxlError(dxl_id, status.error)
        return status

    def send(self, dxl_id, inst, params=b""):
        """Send without waiting for a status (broadcast, sync write)."""
        self.flush()
        self.write_raw(packet(dxl_id, inst, params))

    # --- instructions -----------------------------------------------------------

    def ping(self, dxl_id, timeout=0.5):
        """(model number, firmware version)."""
        return struct.unpack("<HB", self.transact(dxl_id, INST_PING, timeout=timeout).params)

    def read(self, dxl_id, reg, timeout=0.5):
        addr, size = reg
        data = self.transact(dxl_id, INST_READ, struct.pack("<HH", addr, size), timeout).params
        return int.from_bytes(data[:size], "little", signed=reg in SIGNED)

    def write(self, dxl_id, reg, value, timeout=0.5, check=True):
        """Write a register; returns the status (error byte in .error)."""
        addr, size = reg
        data = struct.pack("<H", addr) + int(value).to_bytes(size, "little", signed=value < 0)
        return self.transact(dxl_id, INST_WRITE, data, timeout, check)

    def reboot(self, dxl_id):
        return self.transact(dxl_id, INST_REBOOT)

    def factory_reset(self, dxl_id, option=0xFF):
        """0xFF all, 0x01 all but ID, 0x02 all but ID and Baud Rate."""
        return self.transact(dxl_id, INST_FACTORY_RESET, bytes([option]))

    def sync_read(self, ids, reg, timeout=0.5):
        """{id: value} of one register of several devices."""
        addr, size = reg
        self.send(BROADCAST_ID, INST_SYNC_READ, struct.pack("<HH", addr, size) + bytes(ids))
        values = {}
        for _ in ids:
            status = self.receive(timeout)
            if status is None:
                raise TimeoutError(f"sync read: answers from {sorted(values)} only")
            values[status.id] = int.from_bytes(status.params[:size], "little",
                                               signed=reg in SIGNED)
        return values

    def sync_write(self, reg, values):
        """Write one register of several devices: {id: value}."""
        addr, size = reg
        data = struct.pack("<HH", addr, size)
        for dxl_id, value in values.items():
            data += bytes([dxl_id]) + int(value).to_bytes(size, "little", signed=value < 0)
        self.send(BROADCAST_ID, INST_SYNC_WRITE, data)
