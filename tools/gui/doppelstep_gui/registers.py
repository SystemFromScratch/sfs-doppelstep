# SPDX-License-Identifier: Apache-2.0
"""Control Table of the emulated XM430-W350 (Protocol 2.0).

Addresses, sizes, areas, access and signedness: models/x430_family.yaml of the
DYNAMIXEL X430 Control Table Emulator, "current" variant (XM430). Indirect
addresses are left out. Bus Watchdog is read as signed: the device reports
its error as -1 (tests/sil/test_bus.py).

Units are given only where this repository fixes them (tests/support.py,
tests/sil): position 4096 pulse/rev, velocity 0.229 rev/min, current 2.69 mA,
input voltage 0.1 V, Realtime Tick 1 ms.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class Register:
    name: str
    address: int
    size: int
    area: str            # "eeprom" | "ram"
    access: str          # "r" | "rw"
    signed: bool = False
    unit: str = ""       # unit of the scaled value
    scale: float = 1.0   # physical value = raw * scale

    @property
    def writable(self):
        return self.access == "rw"

    def decode(self, data):
        return int.from_bytes(bytes(data[:self.size]), "little", signed=self.signed)

    def encode(self, value):
        value = int(value)
        return value.to_bytes(self.size, "little", signed=value < 0)

    def scaled(self, raw):
        """Physical value as text, or the raw value when the register has no unit."""
        if not self.unit:
            return str(raw)
        value = raw * self.scale
        if self.unit == "deg":
            return f"{value:.1f}°"
        return f"{value:.3g} {self.unit}" if abs(value) < 1000 else f"{value:.0f} {self.unit}"


DEG_PER_PULSE = 360.0 / 4096
RPM_PER_UNIT = 0.229
MA_PER_UNIT = 2.69
V_PER_UNIT = 0.1

R = Register
TABLE = [
    # EEPROM
    R("Model Number", 0, 2, "eeprom", "r"),
    R("Model Information", 2, 4, "eeprom", "r"),
    R("Firmware Version", 6, 1, "eeprom", "r"),
    R("ID", 7, 1, "eeprom", "rw"),
    R("Baud Rate", 8, 1, "eeprom", "rw"),
    R("Return Delay Time", 9, 1, "eeprom", "rw"),
    R("Drive Mode", 10, 1, "eeprom", "rw"),
    R("Operating Mode", 11, 1, "eeprom", "rw"),
    R("Secondary(Shadow) ID", 12, 1, "eeprom", "rw"),
    R("Protocol Type", 13, 1, "eeprom", "rw"),
    R("Homing Offset", 20, 4, "eeprom", "rw", signed=True),
    R("Moving Threshold", 24, 4, "eeprom", "rw"),
    R("Temperature Limit", 31, 1, "eeprom", "rw", unit="°C"),
    R("Max Voltage Limit", 32, 2, "eeprom", "rw", unit="V", scale=V_PER_UNIT),
    R("Min Voltage Limit", 34, 2, "eeprom", "rw", unit="V", scale=V_PER_UNIT),
    R("PWM Limit", 36, 2, "eeprom", "rw"),
    R("Current Limit", 38, 2, "eeprom", "rw", unit="mA", scale=MA_PER_UNIT),
    R("Velocity Limit", 44, 4, "eeprom", "rw", unit="rpm", scale=RPM_PER_UNIT),
    R("Max Position Limit", 48, 4, "eeprom", "rw", unit="deg", scale=DEG_PER_PULSE),
    R("Min Position Limit", 52, 4, "eeprom", "rw", unit="deg", scale=DEG_PER_PULSE),
    R("Startup Configuration", 60, 1, "eeprom", "rw"),
    R("Shutdown", 63, 1, "eeprom", "rw"),
    # RAM
    R("Torque Enable", 64, 1, "ram", "rw"),
    R("LED", 65, 1, "ram", "rw"),
    R("Status Return Level", 68, 1, "ram", "rw"),
    R("Registered Instruction", 69, 1, "ram", "r"),
    R("Hardware Error Status", 70, 1, "ram", "r"),
    R("Velocity I Gain", 76, 2, "ram", "rw"),
    R("Velocity P Gain", 78, 2, "ram", "rw"),
    R("Position D Gain", 80, 2, "ram", "rw"),
    R("Position I Gain", 82, 2, "ram", "rw"),
    R("Position P Gain", 84, 2, "ram", "rw"),
    R("Feedforward 2nd Gain", 88, 2, "ram", "rw"),
    R("Feedforward 1st Gain", 90, 2, "ram", "rw"),
    R("Bus Watchdog", 98, 1, "ram", "rw", signed=True),
    R("Goal PWM", 100, 2, "ram", "rw", signed=True),
    R("Goal Current", 102, 2, "ram", "rw", signed=True, unit="mA", scale=MA_PER_UNIT),
    R("Goal Velocity", 104, 4, "ram", "rw", signed=True, unit="rpm", scale=RPM_PER_UNIT),
    R("Profile Acceleration", 108, 4, "ram", "rw"),
    R("Profile Velocity", 112, 4, "ram", "rw"),
    R("Goal Position", 116, 4, "ram", "rw", signed=True, unit="deg", scale=DEG_PER_PULSE),
    R("Realtime Tick", 120, 2, "ram", "r", unit="ms"),
    R("Moving", 122, 1, "ram", "r"),
    R("Moving Status", 123, 1, "ram", "r"),
    R("Present PWM", 124, 2, "ram", "r", signed=True),
    R("Present Current", 126, 2, "ram", "r", signed=True, unit="mA", scale=MA_PER_UNIT),
    R("Present Velocity", 128, 4, "ram", "r", signed=True, unit="rpm", scale=RPM_PER_UNIT),
    R("Present Position", 132, 4, "ram", "r", signed=True, unit="deg", scale=DEG_PER_PULSE),
    R("Velocity Trajectory", 136, 4, "ram", "r", signed=True),
    R("Position Trajectory", 140, 4, "ram", "r", signed=True),
    R("Present Input Voltage", 144, 2, "ram", "r", unit="V", scale=V_PER_UNIT),
    R("Present Temperature", 146, 1, "ram", "r", unit="°C"),
    R("Backup Ready", 147, 1, "ram", "r"),
]
del R

BY_NAME = {reg.name: reg for reg in TABLE}

# Short names used by the rest of the GUI
MODEL_NUMBER = BY_NAME["Model Number"]
FIRMWARE_VERSION = BY_NAME["Firmware Version"]
ID = BY_NAME["ID"]
BAUD_RATE = BY_NAME["Baud Rate"]
DRIVE_MODE = BY_NAME["Drive Mode"]
OPERATING_MODE = BY_NAME["Operating Mode"]
TORQUE_ENABLE = BY_NAME["Torque Enable"]
HARDWARE_ERROR_STATUS = BY_NAME["Hardware Error Status"]
BUS_WATCHDOG = BY_NAME["Bus Watchdog"]
GOAL_VELOCITY = BY_NAME["Goal Velocity"]
PROFILE_ACCELERATION = BY_NAME["Profile Acceleration"]
PROFILE_VELOCITY = BY_NAME["Profile Velocity"]
GOAL_POSITION = BY_NAME["Goal Position"]
REALTIME_TICK = BY_NAME["Realtime Tick"]
MOVING = BY_NAME["Moving"]
MOVING_STATUS = BY_NAME["Moving Status"]
PRESENT_CURRENT = BY_NAME["Present Current"]
PRESENT_VELOCITY = BY_NAME["Present Velocity"]
PRESENT_POSITION = BY_NAME["Present Position"]
PRESENT_INPUT_VOLTAGE = BY_NAME["Present Input Voltage"]
PRESENT_TEMPERATURE = BY_NAME["Present Temperature"]
TEMPERATURE_LIMIT = BY_NAME["Temperature Limit"]

# Baud Rate (8) codes: drivers/x430/x430_servo.c (ROBOTIS e-Manual)
BAUD_RATES = {0: 9600, 1: 57600, 2: 115200, 3: 1000000, 4: 2000000, 5: 3000000,
              6: 4000000, 7: 4500000}

# Operating Mode (11) values used by the tests (tests/sil/test_motion.py)
OPERATING_MODES = {1: "velocity control", 3: "position control"}

# Hardware Error Status bits checked by the firmware (tests/sil/test_protection.py)
HW_ERROR_BITS = {0x01: "Input Voltage Error", 0x04: "Overheating Error",
                 0x10: "Electrical Shock Error"}

XM430_W350_MODEL = 1020


def hw_error_text(value):
    if not value:
        return "none"
    names = [name for bit, name in HW_ERROR_BITS.items() if value & bit]
    other = value & ~sum(HW_ERROR_BITS)
    if other:
        names.append(f"bits 0x{other:02x}")
    return ", ".join(names)


def block(first, last):
    """(address, size) of one Read that covers the registers `first` .. `last`."""
    return first.address, last.address + last.size - first.address
