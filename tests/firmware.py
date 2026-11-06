# SPDX-License-Identifier: Apache-2.0
"""The native_sim firmware under test: process, bus PTY and model log."""

import os
import re
import subprocess
import threading
import time
from dataclasses import dataclass

ANSI = re.compile(r"\x1b\[[0-9;]*m")
BUS_PTY = re.compile(r"uart_1 connected to pseudotty: (\S+)")
TIMESTAMP = re.compile(r"^\[(\d+):(\d+):(\d+)\.(\d+),(\d+)\]")
MODEL = re.compile(
    r"sim_plant: cmd (-?\d+) rotor (-?\d+) usteps \(lag (-?\d+) deg el\), (-?\d+) rpm, "
    r"iA (-?\d+) iB (-?\d+) mA, vA (-?\d+) vB (-?\d+) mV, load (-?\d+) uNm")


@dataclass
class ModelSample:
    """One log line of the simulated motor (CONFIG_SFS_SIM_LOG_PERIOD_MS)."""

    t: float          # firmware uptime [s]
    cmd: int          # commanded position [microsteps]
    rotor: int        # rotor position of the model [microsteps]
    lag: int          # rotor behind the command [electrical degrees]
    rpm: int          # rotor speed [rpm]
    i_a: int          # phase currents [mA]
    i_b: int
    v_a: int          # phase voltages commanded [mV]
    v_b: int
    load: int         # load torque [uN*m]

    @property
    def i_amp(self):
        return (self.i_a ** 2 + self.i_b ** 2) ** 0.5


def uptime(line):
    m = TIMESTAMP.match(line)
    if not m:
        return None
    h, mi, s, ms, us = (int(g) for g in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1e3 + us / 1e6


class NativeFirmware:
    """zephyr.exe in its own working directory (flash.bin = emulated EEPROM)."""

    def __init__(self, exe, workdir, args=()):
        self.exe = os.path.abspath(exe)
        self.workdir = workdir
        self.args = list(args)
        self.proc = None
        self.lines = []
        self.pty = None
        self._cond = threading.Condition()
        self._reader = None

    def start(self, timeout=20.0):
        os.makedirs(self.workdir, exist_ok=True)
        self.lines = []
        self.pty = None
        self.proc = subprocess.Popen([self.exe] + self.args, cwd=self.workdir,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self.wait_for(BUS_PTY, timeout)
        self.wait_for(re.compile(r"x430_servo: DYNAMIXEL"), timeout)
        return self

    def _read(self):
        for line in self.proc.stdout:
            line = ANSI.sub("", line.rstrip("\n"))
            with self._cond:
                self.lines.append(line)
                m = BUS_PTY.search(line)
                if m:
                    self.pty = m.group(1)
                self._cond.notify_all()
        with self._cond:
            self._cond.notify_all()

    def wait_for(self, regex, timeout=10.0, start=0):
        """First match of `regex` in the output from line `start` on."""
        deadline = time.monotonic() + timeout
        with self._cond:
            index = start
            while True:
                while index < len(self.lines):
                    m = regex.search(self.lines[index])
                    if m:
                        return m
                    index += 1
                left = deadline - time.monotonic()
                if left <= 0 or self.proc.poll() is not None and index >= len(self.lines):
                    raise TimeoutError(f"no line matching {regex.pattern!r}; last lines:\n"
                                       + "\n".join(self.lines[-15:]))
                self._cond.wait(left)

    def mark(self):
        """Index of the next output line (for wait_for(start=...) / samples(since=...))."""
        with self._cond:
            return len(self.lines)

    def samples(self, since=0):
        """Model log lines from line `since` on."""
        with self._cond:
            lines = self.lines[since:]
        out = []
        for line in lines:
            m = MODEL.search(line)
            if m:
                out.append(ModelSample(uptime(line), *(int(g) for g in m.groups())))
        return out

    def next_sample(self, timeout=2.0):
        """The first model log line written after this call."""
        m = self.wait_for(MODEL, timeout, start=self.mark())
        return ModelSample(uptime(m.string), *(int(g) for g in m.groups()))

    def errors(self):
        with self._cond:
            return [l for l in self.lines if "<err>" in l]

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self._reader:
            self._reader.join(2)

    def restart(self, timeout=20.0):
        """Stop and start again in the same working directory (EEPROM kept)."""
        self.stop()
        return self.start(timeout)
