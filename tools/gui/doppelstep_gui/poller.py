# SPDX-License-Identifier: Apache-2.0
"""Live registers: one Read of the RAM block every period, and the CSV recording."""

import csv
import threading
import time
from dataclasses import dataclass

from . import registers as r

FIRST = r.TORQUE_ENABLE
LAST = r.PRESENT_TEMPERATURE
BLOCK_ADDRESS, BLOCK_SIZE = r.block(FIRST, LAST)

LIVE = [r.TORQUE_ENABLE, r.HARDWARE_ERROR_STATUS, r.GOAL_POSITION, r.MOVING, r.MOVING_STATUS,
        r.PRESENT_CURRENT, r.PRESENT_VELOCITY, r.PRESENT_POSITION, r.PRESENT_INPUT_VOLTAGE,
        r.PRESENT_TEMPERATURE, r.REALTIME_TICK]


@dataclass
class Sample:
    t: float                 # host time since the start of the poller [s]
    values: dict             # register name -> raw value

    def physical(self):
        """The columns of the plots and the CSV file, in physical units."""
        v = self.values
        return {
            "time_s": round(self.t, 4),
            "goal_position_deg": v["Goal Position"] * r.DEG_PER_PULSE,
            "present_position_deg": v["Present Position"] * r.DEG_PER_PULSE,
            "present_velocity_rpm": v["Present Velocity"] * r.RPM_PER_UNIT,
            "present_current_ma": v["Present Current"] * r.MA_PER_UNIT,
            "input_voltage_v": v["Present Input Voltage"] * r.V_PER_UNIT,
            "torque_enable": v["Torque Enable"],
            "moving": v["Moving"],
            "hardware_error_status": v["Hardware Error Status"],
            "realtime_tick_ms": v["Realtime Tick"],
            "temperature_c": v["Present Temperature"],
        }


def decode_block(data):
    return {reg.name: reg.decode(data[reg.address - BLOCK_ADDRESS:]) for reg in LIVE}


class Poller:
    """Thread that reads the live block and hands the samples to `on_sample`.

    `on_sample(sample)` and `on_error(exc)` run in the poller thread: the GUI
    puts them on its queue.
    """

    def __init__(self, bus, dxl_id, on_sample, on_error, period=0.1):
        self.bus = bus
        self.dxl_id = dxl_id
        self.on_sample = on_sample
        self.on_error = on_error
        self.period = period
        self.paused = threading.Event()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="poller", daemon=True)
        self._t0 = time.monotonic()

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread.is_alive() and threading.current_thread() is not self._thread:
            self._thread.join(timeout=2.0)

    def _run(self):
        next_time = time.monotonic()
        while not self._stop.is_set():
            if not self.paused.is_set():
                try:
                    data = self.bus.read_block(self.dxl_id, BLOCK_ADDRESS, BLOCK_SIZE,
                                               timeout=0.1)
                    self.on_sample(Sample(time.monotonic() - self._t0, decode_block(data)))
                except Exception as exc:  # noqa: BLE001 - shown in the GUI
                    self.on_error(exc)
            next_time += self.period
            delay = next_time - time.monotonic()
            if delay < 0:
                next_time = time.monotonic()
                delay = 0
            self._stop.wait(delay)


class CsvRecorder:
    """Writes the physical values of every sample to a CSV file."""

    COLUMNS = list(Sample(0.0, {reg.name: 0 for reg in LIVE}).physical())

    def __init__(self, path):
        self.path = path
        self._file = open(path, "w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=self.COLUMNS)
        self._writer.writeheader()
        self.rows = 0

    def add(self, sample):
        row = sample.physical()
        self._writer.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v)
                               for k, v in row.items()})
        self.rows += 1

    def close(self):
        self._file.close()
