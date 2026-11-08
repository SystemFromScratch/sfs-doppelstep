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
import sys
import time

# Packets, CRC and errors: shared with the GUI (tools/gui/doppelstep_gui/protocol.py)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "gui"))

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
REALTIME_TICK = (120, 2)
MOVING = (122, 1)
MOVING_STATUS = (123, 1)
PRESENT_CURRENT = (126, 2)
PRESENT_VELOCITY = (128, 4)
PRESENT_POSITION = (132, 4)
PRESENT_INPUT_VOLTAGE = (144, 2)

SIGNED = {HOMING_OFFSET, GOAL_VELOCITY, GOAL_POSITION, PRESENT_CURRENT, PRESENT_VELOCITY,
          PRESENT_POSITION, BUS_WATCHDOG}

from doppelstep_gui.protocol import (  # noqa: E402,F401  (re-exported for the tests)
    ALERT, BROADCAST_ID, ERR_ACCESS, ERR_CRC, ERR_DATA_LENGTH, ERR_DATA_LIMIT, ERR_DATA_RANGE,
    ERR_INSTRUCTION, ERR_RESULT_FAIL, INST_CLEAR, INST_FACTORY_RESET, INST_PING, INST_READ,
    INST_REBOOT, INST_STATUS, INST_SYNC_READ, INST_SYNC_WRITE, INST_WRITE, CLEAR_MULTI_TURN,
    DxlError, Status, crc16, packet, stuff, take_status)


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
            status = take_status(self.rx)
            if status is not None:
                return status
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

    def clear_multi_turn(self, dxl_id):
        """Clear instruction: Present Position back into one turn."""
        return self.transact(dxl_id, INST_CLEAR, CLEAR_MULTI_TURN)

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
