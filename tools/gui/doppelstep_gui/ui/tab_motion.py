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


# Profile Acceleration unit of the X430 [rev/min^2]
ACCEL_RPM2_PER_UNIT = 214.577


class MotionTab(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=8)
        self.app = app
        self.recorder = None
        self.samples = collections.deque(maxlen=600 * 10)
        self._controls = []
        self._pos_controls = []         # useful in position control (Operating Mode 3)
        self._vel_controls = []         # useful in velocity control (Operating Mode 1)
        self.op_mode = None             # Operating Mode of the device; None = not known
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
        # One button shows the torque state and toggles it: green = on, red = off.
        # tk.Button, not ttk: the native Windows theme ignores a ttk background colour.
        self.torque_button = tk.Button(f, text="Torque: not connected", command=self._toggle_torque,
                                       font=style.big_font(self), relief=tk.RAISED, bd=2,
                                       padx=8, pady=4, cursor="hand2")
        self.torque_button.pack(fill=tk.X)
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
        send = self._button(row, "Send", lambda: self._write(r.GOAL_POSITION, self.goal_var.get()),
                            style="Accent.TButton")
        send.pack(side=tk.LEFT, padx=4)
        self._pos_controls.append(send)
        quick = ttk.Frame(f)
        quick.pack(fill=tk.X, pady=(6, 0))
        for text, value in (("−90°", START_POSITION - 1024), ("0°", START_POSITION),
                            ("+90°", START_POSITION + 1024)):
            button = self._button(quick, text, lambda v=value: self._goto(v, scale), width=6)
            button.pack(side=tk.LEFT, padx=(0, 4))
            self._pos_controls.append(button)
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
        # Velocity-based profile: the largest speed and the acceleration of a move.
        # Time-based (Drive Mode bit 2): the move time and the ramp time in ms.
        for label, var, unit, factor in (
                ("Profile Velocity", self.pv_var, "rev/min", r.RPM_PER_UNIT),
                ("Profile Acceleration", self.pa_var, "rev/min²", ACCEL_RPM2_PER_UNIT)):
            row = ttk.Frame(f)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label, width=18).pack(side=tk.LEFT)
            ttk.Spinbox(row, from_=0, to=32767, textvariable=var, width=7).pack(side=tk.LEFT)
            shown = ttk.Label(row, text="", style="Muted.TLabel")
            shown.pack(side=tk.LEFT, padx=4)

            def show(*_args, var=var, shown=shown, unit=unit, factor=factor):
                try:
                    value = int(var.get())
                except (tk.TclError, ValueError):
                    return
                shown.configure(text="= maximum" if value == 0 else
                                f"= {value * factor:.4g} {unit}")
            var.trace_add("write", show)
            show()
        self._button(f, "Apply profile", self._apply_profile).pack(anchor="w", pady=(4, 0))
        ttk.Label(f, text="Time-based profile (Drive Mode 4): both in milliseconds.",
                  style="Muted.TLabel").pack(anchor="w", pady=(2, 0))

    def _build_velocity(self, parent):
        f = self._frame(parent, "Goal Velocity (velocity control)")
        row = ttk.Frame(f)
        row.pack(fill=tk.X)
        self.gv_var = tk.IntVar(value=0)
        ttk.Spinbox(row, from_=-1023, to=1023, textvariable=self.gv_var, width=8).pack(
            side=tk.LEFT)
        ttk.Label(row, text="× 0.229 rev/min").pack(side=tk.LEFT, padx=4)
        send = self._button(row, "Send", lambda: self._write(r.GOAL_VELOCITY, self.gv_var.get()))
        send.pack(side=tk.LEFT)
        zero = self._button(row, "0", lambda: self._write(r.GOAL_VELOCITY, 0), width=3)
        zero.pack(side=tk.LEFT, padx=4)
        self._vel_controls += [send, zero]

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

        def done(_status):
            self.app.log(f"{reg.name} = {value}")
            if reg is r.OPERATING_MODE:
                self._set_mode(value)
            elif reg is r.TORQUE_ENABLE:
                self._set_torque(bool(value))

        self.app.run_bus(work, done, f"Write {reg.name}")

    def _read_mode(self):
        dxl_id = self.app.dxl_id
        self.app.run_bus(lambda bus: bus.read(dxl_id, r.OPERATING_MODE), self._set_mode,
                         "Read Operating Mode")

    def _set_mode(self, mode):
        self.op_mode = int(mode)
        text = r.OPERATING_MODES.get(self.op_mode)
        if text is not None:
            self.mode_var.set(f"{self.op_mode} — {text}")
        self._update_controls()

    def _toggle_torque(self):
        if self.torque:
            self.stop()
        else:
            self._write(r.TORQUE_ENABLE, 1)

    def stop(self):
        if self.app.bus is None:
            return
        dxl_id = self.app.dxl_id

        def done(_status):
            self.app.log("Torque Enable = 0")
            self._set_torque(False)

        self.app.run_bus(lambda bus: bus.write(dxl_id, r.TORQUE_ENABLE, 0), done, "Torque off")

    def _set_torque(self, on):
        """Show a written torque state at once; the live registers confirm it."""
        self.torque = on
        self._update_controls()

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
            self.op_mode = None
        self._update_controls()
        if connected:
            self._read_mode()

    def on_busy(self, name):
        self._update_controls()

    def _update_controls(self):
        enabled = self.app.bus is not None and not self.app.busy
        for widget in self._controls:
            widget.configure(state="normal" if enabled else "disabled")
        # The torque button: state from the live registers, click toggles it
        if self.app.bus is None or self.torque is None:
            text = "Torque: not connected" if self.app.bus is None else "Torque: reading …"
            colours = (style.CARD, style.MUTED)
        elif self.torque:
            text, colours = "● TORQUE ON — click to switch off", (style.GOOD, "white")
        else:
            text, colours = "○ TORQUE OFF — click to switch on", (style.BAD, "white")
        self.torque_button.configure(
            text=text, bg=colours[0], fg=colours[1], activebackground=colours[0],
            activeforeground=colours[1], disabledforeground=colours[1],
            state="normal" if enabled and self.torque is not None else "disabled")
        if enabled and self.torque:
            # The EEPROM (Operating Mode) is locked while the torque is on
            self.mode_button.configure(state="disabled")
        # Goal Position moves the joint in position control only, Goal Velocity in velocity
        # control only: the device stores the other one and does nothing with it
        if enabled and self.op_mode is not None:
            for widget in self._pos_controls:
                widget.configure(state="normal" if self.op_mode == 3 else "disabled")
            for widget in self._vel_controls:
                widget.configure(state="normal" if self.op_mode == 1 else "disabled")
