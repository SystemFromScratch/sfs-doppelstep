# SPDX-License-Identifier: Apache-2.0
"""A link that answers like a small XM430: enough Control Table logic for the unit tests.

Only for tests of the GUI code. It is not a model of the firmware.
"""

import struct

from doppelstep_gui import protocol as p
from doppelstep_gui import registers as r


class FakeServo:
    def __init__(self, dxl_id=1, model=1020, firmware=1, torque_fails=False):
        self.id = dxl_id
        self.table = bytearray(148)
        struct.pack_into("<H", self.table, 0, model)
        self.table[6] = firmware
        self.table[7] = dxl_id
        self.table[8] = 3
        self.table[11] = 3
        struct.pack_into("<i", self.table, 132, 2048)
        self.torque_fails = torque_fails
        self.out = bytearray()
        self.inbox = bytearray()
        self.reboots = 0
        self.silent = False
        self.closed = False

    # link interface
    def write(self, data):
        self.inbox += data
        while True:
            start = self.inbox.find(p.HEADER)
            if start < 0 or len(self.inbox) < start + 7:
                return
            length = struct.unpack_from("<H", self.inbox, start + 5)[0]
            end = start + 7 + length
            if len(self.inbox) < end:
                return
            raw = bytes(self.inbox[start:end])
            del self.inbox[:end]
            if struct.unpack_from("<H", raw, len(raw) - 2)[0] != p.crc16(raw[:-2]):
                continue
            self._handle(raw[4], raw[7], p.unstuff(raw[8:-2]))

    def read(self, timeout):
        data, self.out = bytes(self.out), bytearray()
        return data

    def discard_input(self):
        self.out.clear()

    def close(self):
        self.closed = True

    # servo
    def _status(self, error=0, params=b""):
        if not self.silent:
            self.out += p.packet(self.id, p.INST_STATUS, bytes([error]) + params)

    def _handle(self, dxl_id, inst, params):
        if inst == p.INST_SYNC_READ:
            addr, size = struct.unpack_from("<HH", params)
            if self.id in params[4:]:
                self._status(0, bytes(self.table[addr:addr + size]))
            return
        if inst == p.INST_SYNC_WRITE:
            addr, size = struct.unpack_from("<HH", params)
            data = params[4:]
            for k in range(0, len(data), size + 1):
                if data[k] == self.id:
                    self.table[addr:addr + size] = data[k + 1:k + 1 + size]
            return
        if dxl_id != self.id:
            return
        if inst == p.INST_PING:
            self._status(0, bytes(self.table[0:2]) + bytes(self.table[6:7]))
        elif inst == p.INST_READ:
            addr, size = struct.unpack_from("<HH", params)
            self._status(0, bytes(self.table[addr:addr + size]))
        elif inst == p.INST_WRITE:
            addr = struct.unpack_from("<H", params)[0]
            data = params[2:]
            if addr == r.TORQUE_ENABLE.address and data[0] == 1 and self.torque_fails:
                self._status(p.ERR_RESULT_FAIL)
                return
            if addr == r.OPERATING_MODE.address and data[0] not in (1, 3):
                self._status(p.ERR_DATA_RANGE)
                return
            if addr < 64 and self.table[64]:
                self._status(p.ERR_ACCESS)
                return
            self._status(0)                      # status from the old ID
            self.table[addr:addr + len(data)] = data
            if addr == r.ID.address:
                self.id = data[0]
        elif inst == p.INST_REBOOT:
            self._status(0)
            self.reboots += 1
            self.table[64:148] = bytes(84)
            struct.pack_into("<i", self.table, 132, 2048)
        elif inst == p.INST_FACTORY_RESET:
            self._status(0)
            self.id = 1
            self.table[7] = 1
            self.table[8] = 3
            self.table[10] = 0
            self.table[11] = 3
        else:
            self._status(p.ERR_INSTRUCTION)
