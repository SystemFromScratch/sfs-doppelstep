# SPDX-License-Identifier: Apache-2.0
"""Serial link to the DYNAMIXEL bus (pyserial): the only module that opens the port."""

import serial
from serial.tools import list_ports


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
        self._serial = serial.serial_for_url(port, baudrate=baudrate, timeout=0,
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
        """Bytes received within `timeout` seconds (returns early once data arrived)."""
        self._serial.timeout = max(0.0, timeout)
        first = self._serial.read(1)
        if not first:
            return b""
        self._serial.timeout = 0
        return first + self._serial.read(max(1, self._serial.in_waiting))

    def discard_input(self):
        self._serial.reset_input_buffer()
