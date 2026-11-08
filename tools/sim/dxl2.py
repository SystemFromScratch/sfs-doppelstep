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

# Packets, CRC, errors and the Control Table: shared with the GUI
# (tools/gui/doppelstep_gui/protocol.py and registers.py, standard library only)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "gui"))

from doppelstep_gui.registers import BY_NAME, TABLE  # noqa: E402


def _reg(name):
    """(address, size) of a register of the XM430-W350 Control Table."""
    return BY_NAME[name].address, BY_NAME[name].size


MODEL_NUMBER = _reg("Model Number")
FIRMWARE_VERSION = _reg("Firmware Version")
ID = _reg("ID")
BAUD_RATE = _reg("Baud Rate")
RETURN_DELAY_TIME = _reg("Return Delay Time")
DRIVE_MODE = _reg("Drive Mode")
OPERATING_MODE = _reg("Operating Mode")
HOMING_OFFSET = _reg("Homing Offset")
CURRENT_LIMIT = _reg("Current Limit")
VELOCITY_LIMIT = _reg("Velocity Limit")
TORQUE_ENABLE = _reg("Torque Enable")
HARDWARE_ERROR_STATUS = _reg("Hardware Error Status")
BUS_WATCHDOG = _reg("Bus Watchdog")
GOAL_VELOCITY = _reg("Goal Velocity")
PROFILE_ACCELERATION = _reg("Profile Acceleration")
PROFILE_VELOCITY = _reg("Profile Velocity")
GOAL_POSITION = _reg("Goal Position")
REALTIME_TICK = _reg("Realtime Tick")
MOVING = _reg("Moving")
MOVING_STATUS = _reg("Moving Status")
PRESENT_CURRENT = _reg("Present Current")
PRESENT_VELOCITY = _reg("Present Velocity")
PRESENT_POSITION = _reg("Present Position")
PRESENT_INPUT_VOLTAGE = _reg("Present Input Voltage")

SIGNED = {(r.address, r.size) for r in TABLE if r.signed}

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
