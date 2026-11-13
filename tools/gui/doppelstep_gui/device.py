# SPDX-License-Identifier: Apache-2.0
"""Protocol 2.0 master on a link: the instructions the GUI and the scenarios use.

Every transaction holds `lock`, so the poller, the panels and a running
scenario can share one port. A scenario holds the lock for its whole run
(`with bus.lock:`) to keep its timing.
"""

import struct
import threading
import time

from . import protocol as p
from .registers import XM430_W350_MODEL


class Bus:
    """DYNAMIXEL master on an open link (link.SerialLink or a test double)."""

    def __init__(self, link, timeout=0.5):
        self.link = link
        self.timeout = timeout
        self.lock = threading.RLock()
        self.rx = bytearray()
        self.last_status = None
        self.on_traffic = None           # callback(direction, data) for the bus log

    def close(self):
        with self.lock:
            self.link.close()

    # --- transport -----------------------------------------------------------

    def write_raw(self, data):
        with self.lock:
            if self.on_traffic:
                self.on_traffic("tx", data)
            self.link.write(data)

    def receive(self, timeout=None):
        """Next status packet, or None after `timeout` seconds."""
        timeout = self.timeout if timeout is None else timeout
        with self.lock:
            deadline = time.monotonic() + timeout
            while True:
                status = p.take_status(self.rx)
                if status is not None:
                    if self.on_traffic:
                        self.on_traffic("rx", status.raw)
                    return status
                left = deadline - time.monotonic()
                # Read even after the deadline: this thread may have been held past it
                # (the GUI thread keeps the GIL while it redraws), and the answer may be
                # waiting in the port. Give up only when nothing more arrived.
                data = self.link.read(max(0.0, left))
                if data:
                    self.rx += data
                elif left <= 0:
                    return None

    def flush(self):
        with self.lock:
            self.rx.clear()
            self.link.discard_input()

    def send(self, dxl_id, inst, params=b""):
        """Send without waiting for a status (broadcast, sync write)."""
        with self.lock:
            self.flush()
            self.write_raw(p.packet(dxl_id, inst, params))

    def transact(self, dxl_id, inst, params=b"", timeout=None, check=True):
        """Send one instruction and return its status (raises on timeout / error)."""
        with self.lock:
            self.send(dxl_id, inst, params)
            status = self.receive(timeout)
            if status is None:
                raise TimeoutError(f"no answer from ID {dxl_id}")
            self.last_status = status
            if check and (status.error & 0x7F):
                raise p.DxlError(dxl_id, status.error)
            return status

    # --- instructions ----------------------------------------------------------

    def ping(self, dxl_id, timeout=None):
        """(model number, firmware version)."""
        return struct.unpack("<HB", self.transact(dxl_id, p.INST_PING, timeout=timeout).params[:3])

    def read_block(self, dxl_id, address, size, timeout=None):
        params = struct.pack("<HH", address, size)
        data = self.transact(dxl_id, p.INST_READ, params, timeout).params
        if len(data) < size:
            raise IOError(f"read of {size} bytes at {address} returned {len(data)}")
        return data[:size]

    def read(self, dxl_id, reg, timeout=None):
        return reg.decode(self.read_block(dxl_id, reg.address, reg.size, timeout))

    def write(self, dxl_id, reg, value, timeout=None, check=True):
        """Write a register; returns the status (error byte in .error)."""
        data = struct.pack("<H", reg.address) + reg.encode(value)
        return self.transact(dxl_id, p.INST_WRITE, data, timeout, check)

    def reboot(self, dxl_id):
        return self.transact(dxl_id, p.INST_REBOOT)

    def clear_multi_turn(self, dxl_id):
        """Clear instruction: Present Position back into one turn."""
        return self.transact(dxl_id, p.INST_CLEAR, p.CLEAR_MULTI_TURN)

    def factory_reset(self, dxl_id, option=0xFF):
        """0xFF all, 0x01 all but ID, 0x02 all but ID and Baud Rate."""
        return self.transact(dxl_id, p.INST_FACTORY_RESET, bytes([option]))

    def sync_read(self, ids, reg, timeout=None):
        """{id: value} of one register of several devices."""
        with self.lock:
            self.send(p.BROADCAST_ID, p.INST_SYNC_READ,
                      struct.pack("<HH", reg.address, reg.size) + bytes(ids))
            values = {}
            for _ in ids:
                status = self.receive(timeout)
                if status is None:
                    raise TimeoutError(f"sync read: answers from {sorted(values)} only")
                values[status.id] = reg.decode(status.params)
            return values

    def sync_write(self, reg, values):
        """Write one register of several devices: {id: value}."""
        data = struct.pack("<HH", reg.address, reg.size)
        for dxl_id, value in values.items():
            data += bytes([dxl_id]) + reg.encode(value)
        self.send(p.BROADCAST_ID, p.INST_SYNC_WRITE, data)

    def wait_for_ping(self, dxl_id, timeout, interval=0.05):
        """Ping until the device answers; return (model, firmware, seconds waited)."""
        start = time.monotonic()
        while True:
            try:
                model, firmware = self.ping(dxl_id, timeout=interval)
                return model, firmware, time.monotonic() - start
            except (TimeoutError, IOError):
                if time.monotonic() - start > timeout:
                    raise TimeoutError(f"ID {dxl_id} did not answer within {timeout} s")


def is_doppelstep(model):
    return model == XM430_W350_MODEL
