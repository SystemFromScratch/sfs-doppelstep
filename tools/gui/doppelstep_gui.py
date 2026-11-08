#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Doppelstep GUI: test bench and firmware update of the B-G431B-ESC1 over the DYNAMIXEL bus.

  python tools/gui/doppelstep_gui.py [--port COM5] [--baud 1000000] [--id 1]

Documentation: docs/gui/index.md.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", help="serial port preselected in the window (e.g. COM5)")
    parser.add_argument("--baud", type=int, default=1000000,
                        help="DYNAMIXEL baud rate in bps (%(default)s)")
    parser.add_argument("--id", type=int, default=1, help="DYNAMIXEL ID (%(default)s)")
    args = parser.parse_args()

    try:
        import tkinter  # noqa: F401
    except ImportError:
        sys.exit("Tkinter is missing: install Python from python.org with \"tcl/tk and IDLE\" "
                 "(Windows) or the python3-tk package (Debian/Ubuntu).")
    try:
        import matplotlib  # noqa: F401
        import serial  # noqa: F401
        import smpclient  # noqa: F401
    except ImportError as exc:
        sys.exit(f"{exc.name} is missing: pip install -r requirements.txt")

    from doppelstep_gui.ui.app import main as run

    run(args.port, args.baud, args.id)


if __name__ == "__main__":
    main()
