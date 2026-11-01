#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DYNAMIXEL Protocol 2.0 smoke test of Doppelstep, without ROS 2.

Pings the emulated XM430-W350, reads its live registers, enables the torque,
sends a Goal Position and follows the motion, then disables the torque. Meant
for the native_sim build (tools/sim/run_native.py), works on any serial port.

  python3 tools/sim/dxl_smoke_test.py build-native/ttyDXL
  python3 tools/sim/dxl_smoke_test.py --tcp localhost:5555

Only the Python standard library is used (protocol client: dxl2.py); a serial
port needs Linux (termios), --tcp works on Windows too.
"""

import argparse
import sys
import time

from dxl2 import (GOAL_POSITION, HARDWARE_ERROR_STATUS, MOVING, PRESENT_CURRENT,
                  PRESENT_INPUT_VOLTAGE, PRESENT_POSITION, PRESENT_VELOCITY, TORQUE_ENABLE, Bus)


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

    bus = Bus(args.port, args.tcp)
    dxl = args.id

    model, firmware = bus.ping(dxl)
    print(f"PING ID {dxl}: model {model}, firmware {firmware}")
    print(f"Present Input Voltage {bus.read(dxl, PRESENT_INPUT_VOLTAGE) / 10:.1f} V, "
          f"Present Position {bus.read(dxl, PRESENT_POSITION)}, "
          f"Hardware Error Status 0x{bus.read(dxl, HARDWARE_ERROR_STATUS):02x}")

    bus.write(dxl, TORQUE_ENABLE, 1)
    print("Torque Enable 1")
    time.sleep(0.2)
    bus.write(dxl, GOAL_POSITION, args.goal)
    print(f"Goal Position {args.goal}")

    end = time.monotonic() + args.time
    while time.monotonic() < end:
        position = bus.read(dxl, PRESENT_POSITION)
        velocity = bus.read(dxl, PRESENT_VELOCITY)
        current = bus.read(dxl, PRESENT_CURRENT)
        moving = bus.read(dxl, MOVING)
        print(f"  position {position:6d}  velocity {velocity:5d}  current {current:5d}  "
              f"moving {moving}")
        time.sleep(0.1)

    bus.write(dxl, TORQUE_ENABLE, 0)
    print("Torque Enable 0")
    final = bus.read(dxl, PRESENT_POSITION)
    print(f"final Present Position {final} (goal {args.goal})")
    return 0 if final == args.goal else 1


if __name__ == "__main__":
    sys.exit(main())
