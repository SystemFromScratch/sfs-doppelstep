# SPDX-License-Identifier: Apache-2.0
"""Firmware update tab: choose the signed image, update, read every step."""

import os
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

from .. import firmware, image, report
from ..device import Bus
from ..link import SerialLink
from . import style

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))))
OUTPUT = os.path.join(ROOT, "output")       # scripts/export_output.py, after every build
BUILD_IMAGE = os.path.join(ROOT, "build", "app", "zephyr", "zephyr.signed.bin")


def default_image():
    """Newest signed image in output/, else the one of the build directory, else ""."""
    try:
        images = [os.path.join(OUTPUT, n) for n in os.listdir(OUTPUT) if n.endswith(".signed.bin")]
    except OSError:
        images = []
    if images:
        return max(images, key=os.path.getmtime)
    return BUILD_IMAGE if os.path.exists(BUILD_IMAGE) else ""


class FirmwareTab(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=8)
        self.app = app
        self.image = None

        f = ttk.LabelFrame(self, text="Signed application image", padding=8)
        f.pack(fill=tk.X)
        row = ttk.Frame(f)
        row.pack(fill=tk.X)
        self.path_var = tk.StringVar(value=default_image())
        ttk.Entry(row, textvariable=self.path_var).pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(row, text="Browse …", command=self._browse).pack(side=tk.LEFT, padx=4)
        ttk.Button(row, text="Reload", command=self._load).pack(side=tk.LEFT)
        self.info = ttk.Label(f, text="", justify="left")
        self.info.pack(anchor="w", pady=(8, 0))
        ttk.Label(f, text="Every board build puts doppelstep-<version>-…-app.signed.bin "
                          "(or …-sim-motor.signed.bin) into output/; the newest is chosen. "
                          "zephyr.bin and zephyr.elf carry no signature: MCUboot would not "
                          "start them.",
                  style="Muted.TLabel").pack(anchor="w", pady=(4, 0))

        g = ttk.LabelFrame(self, text="Update over USART2 (MCUboot serial recovery)", padding=8)
        g.pack(fill=tk.X, pady=8)
        row = ttk.Frame(g)
        row.pack(fill=tk.X)
        self.update_button = ttk.Button(row, text="⭳ Update firmware", style="Accent.TButton",
                                        command=self.start_update)
        self.update_button.pack(side=tk.LEFT)
        self.report_button = ttk.Button(row, text="Save HTML report …", state="disabled",
                                        command=self._save_report)
        self.report_button.pack(side=tk.LEFT, padx=6)
        self.result_label = ttk.Label(row, text="", style="Heading.TLabel")
        self.result_label.pack(side=tk.LEFT, padx=12)
        self.progress = ttk.Progressbar(g, mode="determinate", maximum=100)
        self.progress.pack(fill=tk.X, pady=(8, 2))
        self.progress_label = ttk.Label(g, text="", style="Muted.TLabel")
        self.progress_label.pack(anchor="w")
        ttk.Label(g, text="Sequence: DYNAMIXEL Reboot → SMP echo until MCUboot answers in its "
                          "500 ms window → image list → upload into slot 0 → image list "
                          "(the hash must be the SHA-256 of the file) → reset → the application "
                          "answers the DYNAMIXEL ping. MCUboot always uses 1 Mbps. Without a "
                          "connection the update goes straight to SMP (MCUboot in recovery "
                          "because slot 0 has no valid image).",
                  style="Muted.TLabel", wraplength=1100, justify="left").pack(anchor="w",
                                                                              pady=(6, 0))

        self.steps = ttk.Treeview(self, columns=("result", "detail", "time"),
                                  show="tree headings")
        self.steps.heading("#0", text="Step")
        self.steps.heading("result", text="Result")
        self.steps.heading("detail", text="Detail")
        self.steps.heading("time", text="Time")
        self.steps.column("#0", width=200)
        self.steps.column("result", width=70, anchor="center")
        self.steps.column("detail", width=760)
        self.steps.column("time", width=70, anchor="e")
        for status, colour in style.RESULT_COLOURS.items():
            self.steps.tag_configure(status, foreground=colour)
        self.steps.pack(fill=tk.BOTH, expand=True)
        self._load()

    # --- image ---------------------------------------------------------------------------

    def _browse(self):
        start = os.path.dirname(self.path_var.get()) or (OUTPUT if os.path.isdir(OUTPUT) else ROOT)
        path = filedialog.askopenfilename(title="Signed MCUboot image", initialdir=start,
                                          filetypes=[("Signed image", "*.signed.bin"),
                                                     ("Binary", "*.bin"), ("All", "*.*")])
        if path:
            self.path_var.set(path)
            self._load()

    def _load(self):
        self.image = None
        path = self.path_var.get().strip()
        if not path:
            self.info.configure(text="No image chosen.", style="Muted.TLabel")
            return
        try:
            self.image = image.load(path)
        except (OSError, image.ImageError) as exc:
            self.info.configure(text=f"✘ {exc}", style="Bad.TLabel")
            return
        img = self.image
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(path)))
        fits = (f"fits slot 0 ({img.size / image.SLOT0_SIZE:.0%} of 86 KiB)" if img.fits_slot0
                else "✘ LARGER than slot 0 (86 KiB)")
        self.info.configure(
            text=f"Version {img.version}    {img.size} bytes, {fits}    built {stamp}\n"
                 f"SHA-256 (image TLV): {img.sha256.hex()}",
            style="TLabel" if img.fits_slot0 else "Bad.TLabel")

    # --- update --------------------------------------------------------------------------

    def start_update(self):
        if not self.app.require_idle():
            return
        self._load()
        if self.image is None:
            messagebox.showerror("Doppelstep", "Choose a valid signed image first.")
            return
        port, baud, dxl_id = self.app.port, self.app.baudrate, self.app.dxl_id
        if not port:
            messagebox.showwarning("Doppelstep", "Choose the serial port in the bar at the top.")
            return
        connected = self.app.bus is not None
        text = (f"Update the firmware on {port}?\n\n"
                f"Image version {self.image.version}, {self.image.size} bytes.\n\n"
                + ("The device gets a DYNAMIXEL Reboot: the motor outputs go off and the MCU "
                   "resets. " if connected else
                   "Not connected: the update talks to MCUboot directly (it must already be "
                   "in serial recovery). ")
                + "Do not unplug the board during the upload. The EEPROM values stay.")
        if not messagebox.askokcancel("Firmware update", text, icon="warning"):
            return

        self.steps.delete(*self.steps.get_children())
        self.progress.configure(value=0)
        self.result_label.configure(text="Updating …", style="Heading.TLabel")
        self.app.set_busy("firmware update")
        bus = self.app.detach_bus()
        path = self.path_var.get().strip()
        self.app.log(f"Firmware update: {path}")

        updater = firmware.FirmwareUpdater(
            self.image, path, dxl_id,
            open_bus=lambda: Bus(SerialLink(port, baud)),
            smp_session=lambda: firmware.SmpSession(port),
            log=lambda line: self.app.post(self._on_log, line, updater),
            progress=lambda done, total: self.app.post(self._on_progress, done, total))

        def worker():
            result = updater.run(bus)
            self.app.post(self._finished, updater, result)

        threading.Thread(target=worker, daemon=True).start()

    def _on_log(self, line, updater):
        self.app.log(line)
        self._show_steps(updater.result)

    def _show_steps(self, result):
        self.steps.delete(*self.steps.get_children())
        for step in result.steps:
            status = "PASS" if step.ok else "FAIL"
            self.steps.insert("", tk.END, text=step.name, tags=(status,),
                              values=(status, step.detail, f"{step.seconds:.2f} s"))

    def _on_progress(self, done, total):
        self.progress.configure(value=100 * done / max(total, 1))
        self.progress_label.configure(text=f"{done} / {total} bytes")

    def _finished(self, updater, result):
        self._show_steps(result)
        self.app.last_update = result
        self.app.set_busy(None)
        self.report_button.configure(state="normal")
        if result.ok:
            self.result_label.configure(text="✔ Update verified", style="Good.TLabel")
            self.app.log("Firmware update: PASS")
        else:
            self.result_label.configure(text="✘ Update failed", style="Bad.TLabel")
            self.app.log("Firmware update: FAIL")
        if updater.bus is not None:
            self.app.attach_bus(updater.bus, *updater.identity)

    def _save_report(self):
        default = time.strftime("doppelstep-update-%Y%m%d-%H%M%S.html")
        path = filedialog.asksaveasfilename(title="Save the update report", initialfile=default,
                                            defaultextension=".html",
                                            filetypes=[("HTML", "*.html")])
        if not path:
            return
        report.save(path, report.build([], self.app.connection_info(), self.app.last_update,
                                       title="Doppelstep firmware update report"))
        self.app.log(f"Report saved: {path}")
        if messagebox.askyesno("Doppelstep", "Report saved. Open it in the browser?"):
            webbrowser.open("file://" + os.path.abspath(path))

    def on_connection(self, connected):
        pass

    def on_busy(self, name):
        self.update_button.configure(state="disabled" if name else "normal")
