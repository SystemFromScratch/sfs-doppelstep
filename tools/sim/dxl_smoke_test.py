#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DYNAMIXEL Protocol 2.0 smoke test of Doppelstep, without ROS 2.

Pings the emulated XM430-W350, reads its live registers, enables the torque,
sends a Goal Position and follows the motion, then disables the torque. Meant
for the native_sim build (tools/sim/run_native.py), works on any serial port.

  python3 tools/sim/dxl_smoke_test.py build-native/ttyDXL
  python3 tools/sim/dxl_smoke_test.py --tcp localhost:5555

Only the Python standard library is used; a serial port needs Linux (termios),
--tcp works on Windows too.
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

# Control Table of the XM430-W350 (e-Manual)
TORQUE_ENABLE = 64
HARDWARE_ERROR_STATUS = 70
GOAL_POSITION = 116
MOVING = 122
PRESENT_CURRENT = 126
PRESENT_VELOCITY = 128
PRESENT_POSITION = 132
PRESENT_INPUT_VOLTAGE = 144

INST_PING, INST_READ, INST_WRITE = 0x01, 0x02, 0x03


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


class Link:
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

    def write(self, data):
        if self.sock:
            self.sock.sendall(data)
        else:
            os.write(self.fd, data)

    def read(self, timeout):
        src = self.sock or self.fd
        ready, _, _ = select.select([src], [], [], timeout)
        if not ready:
            return b""
        return self.sock.recv(4096) if self.sock else os.read(self.fd, 4096)

    def transact(self, dxl_id, inst, params=b"", timeout=0.5):
        body = stuff(params)
        packet = bytes([0xFF, 0xFF, 0xFD, 0x00, dxl_id]) + struct.pack("<H", len(body) + 3)
        packet += bytes([inst]) + body
        packet += struct.pack("<H", crc16(packet))
        self.rx.clear()
        self.write(packet)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.rx += self.read(max(0.0, deadline - time.monotonic()))
            start = self.rx.find(b"\xff\xff\xfd\x00")
            if start < 0 or len(self.rx) < start + 7:
                continue
            length = struct.unpack_from("<H", self.rx, start + 5)[0]
            end = start + 7 + length
            if len(self.rx) < end:
                continue
            status = bytes(self.rx[start:end])
            if struct.unpack_from("<H", status, len(status) - 2)[0] != crc16(status[:-2]):
                raise IOError("status packet with a wrong CRC")
            if status[7] != 0x55:
                raise IOError(f"not a status packet: instruction 0x{status[7]:02x}")
            if status[8] & 0x7F:
                raise IOError(f"device error 0x{status[8]:02x}")
            return status[9:-2]
        raise TimeoutError(f"no answer from ID {dxl_id}")

    def ping(self, dxl_id):
        return struct.unpack("<HB", self.transact(dxl_id, INST_PING))

    def read_reg(self, dxl_id, addr, size, signed=False):
        data = self.transact(dxl_id, INST_READ, struct.pack("<HH", addr, size))
        return int.from_bytes(data[:size], "little", signed=signed)

    def write_reg(self, dxl_id, addr, size, value):
        self.transact(dxl_id, INST_WRITE,
                      struct.pack("<H", addr) + value.to_bytes(size, "little", signed=value < 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("port", nargs="?", help="serial port or PTY (for example build-native/ttyDXL)")
    parser.add_argument("--tcp", metavar="HOST:PORT", help="TCP bridge of run_native.py instead")
    parser.add_argument("--id", type=int, default=1, help="DYNAMIXEL ID (%(default)s)")
    parser.add_argument("--goal", type=int, default=3072,
                        help="Goal Position [pulse], 2048 = 0 rad (%(default)s)")
    parser.add_argument("--time", type=float, default=2.0, help="seconds to follow the move")
    args = parser.parse_args()
    if not (args.port or args.tcp):
        parser.error("give a port or --tcp")

    bus = Link(args.port, args.tcp)
    dxl = args.id

    model, firmware = bus.ping(dxl)
    print(f"PING ID {dxl}: model {model}, firmware {firmware}")
    print(f"Present Input Voltage {bus.read_reg(dxl, PRESENT_INPUT_VOLTAGE, 2) / 10:.1f} V, "
          f"Present Position {bus.read_reg(dxl, PRESENT_POSITION, 4, True)}, "
          f"Hardware Error Status 0x{bus.read_reg(dxl, HARDWARE_ERROR_STATUS, 1):02x}")

    bus.write_reg(dxl, TORQUE_ENABLE, 1, 1)
    print("Torque Enable 1")
    time.sleep(0.2)
    bus.write_reg(dxl, GOAL_POSITION, 4, args.goal)
    print(f"Goal Position {args.goal}")

    end = time.monotonic() + args.time
    while time.monotonic() < end:
        position = bus.read_reg(dxl, PRESENT_POSITION, 4, True)
        velocity = bus.read_reg(dxl, PRESENT_VELOCITY, 4, True)
        current = bus.read_reg(dxl, PRESENT_CURRENT, 2, True)
        moving = bus.read_reg(dxl, MOVING, 1)
        print(f"  position {position:6d}  velocity {velocity:5d}  current {current:5d}  "
              f"moving {moving}")
        time.sleep(0.1)

    bus.write_reg(dxl, TORQUE_ENABLE, 1, 0)
    print("Torque Enable 0")
    final = bus.read_reg(dxl, PRESENT_POSITION, 4, True)
    print(f"final Present Position {final} (goal {args.goal})")
    return 0 if final == args.goal else 1


if __name__ == "__main__":
    sys.exit(main())
