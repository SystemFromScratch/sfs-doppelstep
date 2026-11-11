# SPDX-License-Identifier: Apache-2.0
"""Serial link to the DYNAMIXEL bus (pyserial): the only module that opens the port."""

import time

import serial
from serial.tools import list_ports

# Fixed read timeout of the port. The configuration must not change after opening: on
# Windows every change of serial.timeout calls SetCommState, and the ST-LINK Virtual
# COM Port then re-initialises its UART and loses the bytes in flight.
POLL_S = 0.002


def available_ports():
    """[(device, description)] of the serial ports of this computer."""
    return [(p.device, p.description) for p in sorted(list_ports.comports())]


class SerialLink:
    """A serial port at a fixed baud rate, 8N1.

    `port` is a device name (COM5, /dev/ttyACM0) or any pyserial URL.
    """

    def __init__(self, port, baudrate):
        self.port = port
        self.baudrate = baudrate
        self._serial = serial.serial_for_url(port, baudrate=baudrate, timeout=POLL_S,
                                             write_timeout=1.0)

    def close(self):
        if self._serial is not None:
            self._serial.close()
            self._serial = None

    @property
    def is_open(self):
        return self._serial is not None

    def write(self, data):
        self._serial.write(data)
        self._serial.flush()

    def read(self, timeout):
        """Bytes received within `timeout` seconds (returns early once data arrived).

        Waits in steps of POLL_S without touching the port configuration."""
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            waiting = self._serial.in_waiting
            if waiting:
                return self._serial.read(waiting)
            if time.monotonic() >= deadline:
                return b""
            first = self._serial.read(1)          # blocks at most POLL_S
            if first:
                return first + self._serial.read(self._serial.in_waiting)

    def discard_input(self):
        self._serial.reset_input_buffer()
