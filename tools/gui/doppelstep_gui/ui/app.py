# SPDX-License-Identifier: Apache-2.0
"""Main window: connection bar, live status, tabs and the log.

Tk runs only in the main thread. Worker threads (poller, scenarios, update,
single bus operations) hand their results back with `post()`, which the main
loop drains every 50 ms.
"""

import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

from .. import registers as r
from ..device import Bus, is_doppelstep
from ..link import SerialLink, available_ports
from ..poller import Poller
from . import style
from .tab_firmware import FirmwareTab
from .tab_motion import MotionTab
from .tab_table import TableTab
from .tab_tests import TestsTab

BAUD_CHOICES = [f"{bps:,} bps (Baud Rate {code})".replace(",", " ")
                for code, bps in r.BAUD_RATES.items()]


class App(tk.Tk):
    def __init__(self, port=None, baud=1000000, dxl_id=1):
        super().__init__()
        self.title("Doppelstep — test bench and firmware update")
        self.geometry("1280x820")
        self.minsize(1000, 680)
        style.apply(self)

        self.bus = None
        self.poller = None
        self.model = None
        self.firmware_version = None
        self.busy = None                    # name of the running long task, or None
        self.last_update = None             # firmware.UpdateResult of this session
        self.sample_listeners = []
        self._queue = queue.Queue()
        self._poll_errors = 0

        self._build_connection_bar(port, baud, dxl_id)
        self._build_status_bar()

        paned = ttk.PanedWindow(self, orient=tk.VERTICAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))
        self.notebook = ttk.Notebook(paned)
        self.motion = MotionTab(self.notebook, self)
        self.table = TableTab(self.notebook, self)
        self.tests = TestsTab(self.notebook, self)
        self.firmware = FirmwareTab(self.notebook, self)
        self.notebook.add(self.motion, text="  Motion and live plots  ")
        self.notebook.add(self.table, text="  Control Table  ")
        self.notebook.add(self.tests, text="  Test scenarios  ")
        self.notebook.add(self.firmware, text="  Firmware update  ")
        paned.add(self.notebook, weight=5)

        log_frame = ttk.LabelFrame(paned, text="Log")
        self.log_text = tk.Text(log_frame, height=7, wrap="none", font=style.MONO,
                                state="disabled", relief="flat")
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_text.pack(fill=tk.BOTH, expand=True)
        paned.add(log_frame, weight=1)

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(50, self._drain)
        self._set_connected(False)
        self.log("Doppelstep GUI ready. Choose the ST-LINK Virtual COM Port and press Connect.")

    # --- threads -----------------------------------------------------------------------

    def post(self, func, *args):
        """Run `func(*args)` in the Tk thread (callable from any thread)."""
        self._queue.put((func, args))

    def _drain(self):
        try:
            while True:
                func, args = self._queue.get_nowait()
                func(*args)
        except queue.Empty:
            pass
        self.after(50, self._drain)

    def run_bus(self, work, done=None, what="bus operation"):
        """Run `work(bus)` in a worker thread, then `done(result)` in the Tk thread."""
        if self.bus is None:
            messagebox.showwarning("Not connected", "Connect to the device first.")
            return
        bus = self.bus

        def worker():
            try:
                result = work(bus)
            except Exception as exc:  # noqa: BLE001 - reported to the operator
                self.post(self.log, f"{what}: {exc}")
                self.post(messagebox.showerror, "Doppelstep", f"{what} failed:\n{exc}")
                return
            if done is not None:
                self.post(done, result)

        threading.Thread(target=worker, daemon=True).start()

    def log(self, text):
        stamp = time.strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        self.log_text.insert(tk.END, f"{stamp}  {text}\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state="disabled")

    def log_from_thread(self, text):
        self.post(self.log, text)

    # --- connection --------------------------------------------------------------------

    def _build_connection_bar(self, port, baud, dxl_id):
        bar = ttk.Frame(self, padding=(8, 8, 8, 4))
        bar.pack(fill=tk.X)
        ttk.Label(bar, text="Port").pack(side=tk.LEFT)
        self.port_var = tk.StringVar(value=port or "")
        self.port_box = ttk.Combobox(bar, textvariable=self.port_var, width=34)
        self.port_box.pack(side=tk.LEFT, padx=(4, 2))
        ttk.Button(bar, text="↻", width=3, command=self.refresh_ports).pack(side=tk.LEFT)

        ttk.Label(bar, text="Baud rate").pack(side=tk.LEFT, padx=(14, 4))
        codes = {bps: code for code, bps in r.BAUD_RATES.items()}
        self.baud_var = tk.StringVar(value=BAUD_CHOICES[codes.get(baud, 3)])
        ttk.Combobox(bar, textvariable=self.baud_var, values=BAUD_CHOICES, state="readonly",
                     width=26).pack(side=tk.LEFT)

        ttk.Label(bar, text="ID").pack(side=tk.LEFT, padx=(14, 4))
        self.id_var = tk.IntVar(value=dxl_id)
        ttk.Spinbox(bar, from_=0, to=252, textvariable=self.id_var, width=5).pack(side=tk.LEFT)

        self.connect_button = ttk.Button(bar, text="Connect", style="Accent.TButton",
                                         command=self.toggle_connection)
        self.connect_button.pack(side=tk.LEFT, padx=(14, 0))
        self.device_label = ttk.Label(bar, text="", style="Muted.TLabel")
        self.device_label.pack(side=tk.LEFT, padx=12)
        self.refresh_ports()

    def refresh_ports(self):
        ports = available_ports()
        self.port_box["values"] = [f"{dev} — {desc}" for dev, desc in ports]
        if not self.port_var.get() and ports:
            stlink = [p for p in ports if "STLink" in p[1] or "ST-Link" in p[1]
                      or "STMicro" in p[1]]
            dev, desc = (stlink or ports)[0]
            self.port_var.set(f"{dev} — {desc}")

    @property
    def port(self):
        return self.port_var.get().split(" — ")[0].strip()

    @property
    def baudrate(self):
        return r.BAUD_RATES[BAUD_CHOICES.index(self.baud_var.get())]

    @property
    def dxl_id(self):
        return int(self.id_var.get())

    def toggle_connection(self):
        if self.bus is not None:
            self.disconnect()
            return
        port, baud, dxl_id = self.port, self.baudrate, self.dxl_id
        if not port:
            messagebox.showwarning("Doppelstep", "Choose a serial port.")
            return
        self.connect_button.configure(state="disabled")
        self.log(f"Connecting to {port} at {baud} bps, ID {dxl_id} …")

        def worker():
            try:
                bus = Bus(SerialLink(port, baud))
            except Exception as exc:  # noqa: BLE001
                self.post(self._connect_failed, f"Cannot open {port}: {exc}", None)
                return
            try:
                model, firmware = bus.ping(dxl_id)
            except Exception as exc:  # noqa: BLE001
                bus.close()
                self.post(self._connect_failed,
                          f"No answer from ID {dxl_id} at {baud} bps on {port} ({exc}).\n\n"
                          "Check the port, the baud rate and the ID. MCUboot answers no "
                          "DYNAMIXEL: if the application does not start, use the Firmware "
                          "update tab without connecting.", None)
                return
            self.post(self.attach_bus, bus, model, firmware)

        threading.Thread(target=worker, daemon=True).start()

    def connection_info(self):
        """Header of the HTML reports."""
        return {
            "Port": self.port,
            "Baud rate": f"{self.baudrate} bps",
            "DYNAMIXEL ID": self.dxl_id,
            "Model Number": self.model if self.model is not None else "not connected",
            "Firmware Version": (self.firmware_version if self.firmware_version is not None
                                 else "not connected"),
        }

    def _connect_failed(self, message, _unused):
        self.connect_button.configure(state="normal")
        self.log(message.split("\n")[0])
        messagebox.showerror("Doppelstep", message)

    def attach_bus(self, bus, model, firmware):
        """Use an open bus (after Connect or after a firmware update)."""
        self.bus = bus
        self.model, self.firmware_version = model, firmware
        self.connect_button.configure(state="normal")
        self._set_connected(True)
        text = f"model {model}" + (" (XM430-W350)" if is_doppelstep(model) else
                                   " — not an XM430-W350") + f", firmware {firmware}"
        self.log(f"Connected: ID {self.dxl_id}, {text}")
        if not is_doppelstep(model):
            messagebox.showwarning("Doppelstep", f"The device reports {text}.")
        self._poll_errors = 0
        self.poller = Poller(bus, self.dxl_id, lambda s: self.post(self._on_sample, s),
                             lambda e: self.post(self._on_poll_error, e))
        self.poller.start()
        self.table.read_all()

    def detach_bus(self):
        """Stop the poller and hand the open bus over (firmware update); None if none."""
        bus = self.bus
        if self.poller is not None:
            self.poller.stop()
            self.poller = None
        self.bus = None
        self._set_connected(False)
        return bus

    def disconnect(self):
        bus = self.detach_bus()
        if bus is not None:
            bus.close()
            self.log("Disconnected.")

    def _set_connected(self, connected):
        self.connect_button.configure(text="Disconnect" if connected else "Connect")
        if connected:
            self.device_label.configure(
                text=f"● connected — model {self.model}, firmware {self.firmware_version}",
                style="Good.TLabel")
        else:
            self.device_label.configure(text="● not connected", style="Muted.TLabel")
            for key in self.status_values:
                self.status_values[key].configure(text="—", style="Value.TLabel")
        for tab in (getattr(self, name, None) for name in ("motion", "table", "tests",
                                                           "firmware")):
            if tab is not None:
                tab.on_connection(connected)

    # --- busy state ---------------------------------------------------------------------

    def set_busy(self, name):
        """A scenario run or an update owns the bus: pause the poller, lock the panels."""
        self.busy = name
        if self.poller is not None:
            self.poller.paused.set() if name else self.poller.paused.clear()
        for tab in (self.motion, self.table, self.tests, self.firmware):
            tab.on_busy(name)
        self.connect_button.configure(state="disabled" if name else "normal")

    def require_idle(self):
        if self.busy:
            messagebox.showinfo("Doppelstep", f"Wait until the {self.busy} has finished.")
            return False
        return True

    # --- live status --------------------------------------------------------------------

    def _build_status_bar(self):
        bar = ttk.Frame(self, padding=(8, 0, 8, 6))
        bar.pack(fill=tk.X)
        self.status_values = {}
        for key in ("Torque", "Hardware error", "Moving", "Position", "Velocity", "Current",
                    "Input voltage"):
            box = ttk.Frame(bar, style="Card.TFrame", padding=(10, 4))
            box.pack(side=tk.LEFT, padx=(0, 6))
            ttk.Label(box, text=key, style="CardTitle.TLabel").pack(anchor="w")
            value = ttk.Label(box, text="—", style="Value.TLabel")
            value.pack(anchor="w")
            self.status_values[key] = value

    def _on_sample(self, sample):
        if self.bus is None:
            return
        if self._poll_errors:
            self.log("Live registers: the device answers again.")
            self._set_connected(True)
        self._poll_errors = 0
        v = sample.values
        hw = v["Hardware Error Status"]
        sv = self.status_values
        sv["Torque"].configure(text="ON" if v["Torque Enable"] else "off",
                               style="CardWarn.TLabel" if v["Torque Enable"] else "Value.TLabel")
        sv["Hardware error"].configure(text=r.hw_error_text(hw),
                                       style="CardBad.TLabel" if hw else "CardGood.TLabel")
        sv["Moving"].configure(text="yes" if v["Moving"] else "no")
        phys = sample.physical()
        sv["Position"].configure(text=f"{v['Present Position']}  ({phys['present_position_deg']:.1f}°)")
        sv["Velocity"].configure(text=f"{phys['present_velocity_rpm']:.1f} rpm")
        sv["Current"].configure(text=f"{phys['present_current_ma']:.0f} mA")
        sv["Input voltage"].configure(text=f"{phys['input_voltage_v']:.1f} V")
        for listener in self.sample_listeners:
            listener(sample)

    def _on_poll_error(self, exc):
        self._poll_errors += 1
        if self._poll_errors == 1:
            self.log(f"Live registers: {exc}")
        if self._poll_errors == 3:
            self.device_label.configure(text="● connected — no answer", style="Bad.TLabel")

    # --- closing -----------------------------------------------------------------------

    def _on_close(self):
        if self.busy and not messagebox.askyesno(
                "Doppelstep", f"The {self.busy} is still running. Quit anyway?"):
            return
        self.motion.stop_recording()
        self.disconnect()
        self.destroy()


def main(port=None, baud=1000000, dxl_id=1):
    app = App(port, baud, dxl_id)
    app.mainloop()
