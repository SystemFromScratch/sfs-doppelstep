# SPDX-License-Identifier: Apache-2.0
"""Motion tab: torque, goals, profiles, live plots and the CSV recording."""

import collections
import os
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure

from .. import registers as r
from ..poller import CsvRecorder
from ..protocol import ERR_RESULT_FAIL, ERROR_NAMES, DxlError
from ..scenarios import DEAD_TIME_HINT
from . import style

START_POSITION = 2048          # 0 rad for dynamixel_hardware_interface (sfs,x430-servo.yaml)
WINDOWS_S = (10, 30, 60)
# One redraw of the four plots takes about 0.1 s in the Tk thread: 2 per second
# keep the window responsive. The status bar shows every sample (10 per second).
REDRAW_MS = 500


class MotionTab(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=8)
        self.app = app
        self.recorder = None
        self.samples = collections.deque(maxlen=600 * 10)
        self._controls = []
        self._dirty = False

        left = ttk.Frame(self)
        left.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
        self._build_torque(left)
        self._build_position(left)
        self._build_profile(left)
        self._build_velocity(left)

        right = ttk.Frame(self)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self._build_plots(right)
        app.sample_listeners.append(self._on_sample)
        self.after(REDRAW_MS, self._redraw)

    # --- controls ----------------------------------------------------------------------

    def _frame(self, parent, title):
        frame = ttk.LabelFrame(parent, text=title, padding=8)
        frame.pack(fill=tk.X, pady=(0, 8))
        return frame

    def _button(self, parent, text, command, **kw):
        button = ttk.Button(parent, text=text, command=command, **kw)
        self._controls.append(button)
        return button

    def _build_torque(self, parent):
        f = self._frame(parent, "Torque")
        self.torque = None              # last Torque Enable read; None = not known
        self.torque_label = ttk.Label(f, text="Torque: not connected", style="TorqueOff.TLabel")
        self.torque_label.pack(fill=tk.X, pady=(0, 6))
        row = ttk.Frame(f)
        row.pack(fill=tk.X)
        self.torque_on_button = self._button(row, "Torque ON",
                                             lambda: self._write(r.TORQUE_ENABLE, 1))
        self.torque_on_button.pack(side=tk.LEFT, expand=True, fill=tk.X, padx=(0, 4))
        self.stop_button = ttk.Button(row, text="■ STOP (torque off)", style="Stop.TButton",
                                      command=self.stop)
        self.stop_button.pack(side=tk.LEFT, expand=True, fill=tk.X)
        mode = ttk.Frame(f)
        mode.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(mode, text="Operating Mode").pack(side=tk.LEFT)
        self.mode_var = tk.StringVar(value="3 — position control")
        ttk.Combobox(mode, textvariable=self.mode_var, state="readonly", width=22,
                     values=[f"{k} — {v}" for k, v in r.OPERATING_MODES.items()]).pack(
            side=tk.LEFT, padx=4)
        self.mode_button = self._button(mode, "Apply", self._apply_mode)
        self.mode_button.pack(side=tk.LEFT)
        ttk.Label(f, text="EEPROM: write it with the torque off.", style="Muted.TLabel").pack(
            anchor="w", pady=(4, 0))

    def _build_position(self, parent):
        f = self._frame(parent, "Goal Position (position control)")
        self.goal_var = tk.IntVar(value=START_POSITION)
        self.goal_label = ttk.Label(f, text="")
        self.goal_label.pack(anchor="w")
        scale = ttk.Scale(f, from_=0, to=4095, orient=tk.HORIZONTAL, length=300,
                          command=lambda v: self.goal_var.set(int(float(v))))
        scale.set(START_POSITION)
        scale.pack(fill=tk.X, pady=4)
        self.goal_var.trace_add("write", lambda *a: self._show_goal())
        row = ttk.Frame(f)
        row.pack(fill=tk.X)
        ttk.Spinbox(row, from_=0, to=4095, textvariable=self.goal_var, width=7).pack(side=tk.LEFT)
        self._button(row, "Send", lambda: self._write(r.GOAL_POSITION, self.goal_var.get()),
                     style="Accent.TButton").pack(side=tk.LEFT, padx=4)
        quick = ttk.Frame(f)
        quick.pack(fill=tk.X, pady=(6, 0))
        for text, value in (("−90°", START_POSITION - 1024), ("0°", START_POSITION),
                            ("+90°", START_POSITION + 1024)):
            self._button(quick, text, lambda v=value: self._goto(v, scale), width=6).pack(
                side=tk.LEFT, padx=(0, 4))
        self._show_goal()

    def _show_goal(self):
        try:
            value = int(self.goal_var.get())
        except (tk.TclError, ValueError):
            return
        self.goal_label.configure(
            text=f"{value} pulse = {(value - START_POSITION) * r.DEG_PER_PULSE:+.1f}° from 2048")

    def _goto(self, value, scale):
        scale.set(value)
        self.goal_var.set(value)
        self._write(r.GOAL_POSITION, value)

    def _build_profile(self, parent):
        f = self._frame(parent, "Profile")
        self.pv_var = tk.IntVar(value=0)
        self.pa_var = tk.IntVar(value=0)
        for label, var in (("Profile Velocity", self.pv_var),
                           ("Profile Acceleration", self.pa_var)):
            row = ttk.Frame(f)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label, width=20).pack(side=tk.LEFT)
            ttk.Spinbox(row, from_=0, to=32767, textvariable=var, width=8).pack(side=tk.LEFT)
        self._button(f, "Apply profile", self._apply_profile).pack(anchor="w", pady=(4, 0))
        ttk.Label(f, text="Velocity-based profile: 0 = maximum.\n"
                          "Time-based (Drive Mode 4): milliseconds.",
                  style="Muted.TLabel").pack(anchor="w", pady=(4, 0))

    def _build_velocity(self, parent):
        f = self._frame(parent, "Goal Velocity (velocity control)")
        row = ttk.Frame(f)
        row.pack(fill=tk.X)
        self.gv_var = tk.IntVar(value=0)
        ttk.Spinbox(row, from_=-1023, to=1023, textvariable=self.gv_var, width=8).pack(
            side=tk.LEFT)
        ttk.Label(row, text="× 0.229 rev/min").pack(side=tk.LEFT, padx=4)
        self._button(row, "Send", lambda: self._write(r.GOAL_VELOCITY, self.gv_var.get())).pack(
            side=tk.LEFT)
        self._button(row, "0", lambda: self._write(r.GOAL_VELOCITY, 0), width=3).pack(
            side=tk.LEFT, padx=4)

    # --- bus writes ----------------------------------------------------------------------

    def _write(self, reg, value):
        if not self.app.require_idle():
            return
        dxl_id = self.app.dxl_id
        try:
            value = int(value)
        except (tk.TclError, ValueError):
            messagebox.showerror("Doppelstep", f"{reg.name}: not a number")
            return

        def work(bus):
            try:
                return bus.write(dxl_id, reg, value)
            except DxlError as exc:
                torque_refused = (reg is r.TORQUE_ENABLE and value == 1 and
                                  exc.error == ERR_RESULT_FAIL)
                hint = "\n\n" + DEAD_TIME_HINT if torque_refused else ""
                raise RuntimeError(f"{reg.name} = {value} refused: "
                                   f"{ERROR_NAMES.get(exc.error, exc.error)}{hint}") from exc

        self.app.run_bus(work, lambda _s: self.app.log(f"{reg.name} = {value}"),
                         f"Write {reg.name}")

    def stop(self):
        if self.app.bus is None:
            return
        dxl_id = self.app.dxl_id
        self.app.run_bus(lambda bus: bus.write(dxl_id, r.TORQUE_ENABLE, 0),
                         lambda _s: self.app.log("STOP: Torque Enable = 0"), "STOP")

    def _apply_mode(self):
        self._write(r.OPERATING_MODE, int(self.mode_var.get().split(" ")[0]))

    def _apply_profile(self):
        if not self.app.require_idle():
            return
        dxl_id, pv, pa = self.app.dxl_id, self.pv_var.get(), self.pa_var.get()

        def work(bus):
            bus.write(dxl_id, r.PROFILE_ACCELERATION, pa)
            bus.write(dxl_id, r.PROFILE_VELOCITY, pv)

        self.app.run_bus(work, lambda _s: self.app.log(
            f"Profile Acceleration = {pa}, Profile Velocity = {pv}"), "Profile")

    # --- plots ---------------------------------------------------------------------------

    def _build_plots(self, parent):
        top = ttk.Frame(parent)
        top.pack(fill=tk.X)
        ttk.Label(top, text="Live registers", style="Heading.TLabel").pack(side=tk.LEFT)
        self.record_button = ttk.Button(top, text="● Record CSV …", command=self._toggle_record)
        self.record_button.pack(side=tk.LEFT, padx=(16, 4))
        self.record_label = ttk.Label(top, text="10 samples per second", style="Muted.TLabel")
        self.record_label.pack(side=tk.LEFT)
        self.window_var = tk.IntVar(value=WINDOWS_S[0])
        ttk.Label(top, text="Window").pack(side=tk.RIGHT, padx=(0, 4))
        for seconds in reversed(WINDOWS_S):
            ttk.Radiobutton(top, text=f"{seconds} s", value=seconds,
                            variable=self.window_var).pack(side=tk.RIGHT)
        ttk.Button(top, text="Clear", command=self._clear).pack(side=tk.RIGHT, padx=8)

        self.figure = Figure(figsize=(7, 6), dpi=96, layout=None)
        axes = self.figure.subplots(4, 1, sharex=True)
        self.ax_pos, self.ax_vel, self.ax_cur, self.ax_volt = axes
        self.lines = {
            "goal": self.ax_pos.plot([], [], color="#9aa3b5", lw=1.2, ls="--",
                                     label="Goal Position")[0],
            "pos": self.ax_pos.plot([], [], color=style.ACCENT, lw=1.5,
                                    label="Present Position")[0],
            "vel": self.ax_vel.plot([], [], color="#7b3fb5", lw=1.5)[0],
            "cur": self.ax_cur.plot([], [], color="#c0561c", lw=1.5)[0],
            "volt": self.ax_volt.plot([], [], color=style.GOOD, lw=1.5)[0],
        }
        self.ax_pos.set_ylabel("position [°]")
        self.ax_pos.legend(loc="upper left", fontsize=8)
        self.ax_vel.set_ylabel("velocity [rpm]")
        self.ax_cur.set_ylabel("current [mA]")
        self.ax_volt.set_ylabel("input [V]")
        self.ax_volt.set_xlabel("time [s]")
        for ax in axes:
            ax.grid(True, alpha=0.3)
        self.figure.subplots_adjust(left=0.1, right=0.98, top=0.97, bottom=0.08, hspace=0.12)
        self.canvas = FigureCanvasTkAgg(self.figure, master=parent)
        toolbar = NavigationToolbar2Tk(self.canvas, parent, pack_toolbar=False)
        toolbar.update()
        toolbar.pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _on_sample(self, sample):
        torque = bool(sample.values["Torque Enable"])
        if torque != self.torque:
            self.torque = torque
            self._update_controls()
        self.samples.append(sample.physical())
        self._dirty = True
        if self.recorder is not None:
            self.recorder.add(sample)
            if self.recorder.rows % 10 == 0:
                self.record_label.configure(
                    text=f"{self.recorder.rows} rows → {os.path.basename(self.recorder.path)}")

    def _clear(self):
        self.samples.clear()
        self._dirty = True

    def _redraw(self):
        if self._dirty and self.winfo_ismapped():
            self._dirty = False
            window = self.window_var.get()
            data = [s for s in self.samples
                    if not self.samples or s["time_s"] >= self.samples[-1]["time_s"] - window]
            t = [s["time_s"] for s in data]
            series = {"goal": "goal_position_deg", "pos": "present_position_deg",
                      "vel": "present_velocity_rpm", "cur": "present_current_ma",
                      "volt": "input_voltage_v"}
            for key, column in series.items():
                self.lines[key].set_data(t, [s[column] for s in data])
            for ax in (self.ax_pos, self.ax_vel, self.ax_cur, self.ax_volt):
                ax.relim()
                ax.autoscale_view()
            if t:
                self.ax_volt.set_xlim(max(t[-1] - window, t[0]), max(t[-1], t[0] + 1))
            self.canvas.draw_idle()
        self.after(REDRAW_MS, self._redraw)

    # --- recording -----------------------------------------------------------------------

    def _toggle_record(self):
        if self.recorder is not None:
            self.stop_recording()
            return
        default = time.strftime("doppelstep-live-%Y%m%d-%H%M%S.csv")
        path = filedialog.asksaveasfilename(title="Record the live registers to",
                                            defaultextension=".csv", initialfile=default,
                                            filetypes=[("CSV", "*.csv")])
        if not path:
            return
        self.recorder = CsvRecorder(path)
        self.record_button.configure(text="■ Stop recording")
        self.app.log(f"Recording the live registers to {path}")

    def stop_recording(self):
        if self.recorder is None:
            return
        self.recorder.close()
        self.app.log(f"Recording stopped: {self.recorder.rows} rows in {self.recorder.path}")
        self.record_label.configure(text=f"Saved {self.recorder.rows} rows to "
                                         f"{os.path.basename(self.recorder.path)}")
        self.recorder = None
        self.record_button.configure(text="● Record CSV …")

    # --- state ---------------------------------------------------------------------------

    def on_connection(self, connected):
        if not connected:
            self.torque = None
        self._update_controls()

    def on_busy(self, name):
        self._update_controls()

    def _update_controls(self):
        enabled = self.app.bus is not None and not self.app.busy
        for widget in self._controls:
            widget.configure(state="normal" if enabled else "disabled")
        self.stop_button.configure(state="normal" if self.app.bus is not None and
                                   not self.app.busy else "disabled")
        # The torque state from the live registers
        if self.app.bus is None:
            self.torque_label.configure(text="Torque: not connected", style="TorqueOff.TLabel")
        elif self.torque is None:
            self.torque_label.configure(text="Torque: reading …", style="TorqueOff.TLabel")
        elif self.torque:
            self.torque_label.configure(text="● TORQUE ON — the motor is powered",
                                        style="TorqueOn.TLabel")
        else:
            self.torque_label.configure(text="○ Torque off — the motor is free",
                                        style="TorqueOff.TLabel")
        if enabled and self.torque:
            # Already on; the EEPROM (Operating Mode) is locked while the torque is on
            self.torque_on_button.configure(text="✔ Torque is ON", style="TorqueActive.TButton",
                                            state="disabled")
            self.mode_button.configure(state="disabled")
        else:
            self.torque_on_button.configure(text="Torque ON", style="TButton")
