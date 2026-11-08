# SPDX-License-Identifier: Apache-2.0
"""Control Table tab: every register, read in one block, written by double-click."""

import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

from .. import protocol as p
from .. import registers as r

TABLE_END = r.TABLE[-1].address + r.TABLE[-1].size


class TableTab(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=8)
        self.app = app
        self._auto_job = None

        bar = ttk.Frame(self)
        bar.pack(fill=tk.X, pady=(0, 6))
        self.read_button = ttk.Button(bar, text="Read all", style="Accent.TButton",
                                      command=self.read_all)
        self.read_button.pack(side=tk.LEFT)
        self.auto_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="Refresh every second", variable=self.auto_var,
                        command=self._auto).pack(side=tk.LEFT, padx=10)
        self.write_button = ttk.Button(bar, text="Write selected …", command=self.write_selected)
        self.write_button.pack(side=tk.LEFT)
        ttk.Label(bar, text="Double-click a writable register to write it. EEPROM registers "
                            "(address < 64) are locked while the torque is on.",
                  style="Muted.TLabel").pack(side=tk.LEFT, padx=10)

        columns = ("address", "name", "size", "area", "access", "raw", "value")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", selectmode="browse")
        for col, text, width, anchor in (("address", "Address", 70, "e"),
                                         ("name", "Name", 220, "w"),
                                         ("size", "Size", 50, "e"),
                                         ("area", "Area", 70, "w"),
                                         ("access", "Access", 60, "w"),
                                         ("raw", "Raw value", 120, "e"),
                                         ("value", "Value", 160, "e")):
            self.tree.heading(col, text=text)
            self.tree.column(col, width=width, anchor=anchor, stretch=col == "name")
        self.tree.tag_configure("eeprom", background="#f6f3ea")
        self.tree.tag_configure("readonly", foreground="#5f6673")
        for reg in r.TABLE:
            tags = [reg.area] + ([] if reg.writable else ["readonly"])
            self.tree.insert("", tk.END, iid=reg.name, tags=tags,
                             values=(reg.address, reg.name, reg.size, reg.area.upper(),
                                     reg.access.upper(), "", ""))
        scroll = ttk.Scrollbar(self, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.pack(fill=tk.BOTH, expand=True)
        self.tree.bind("<Double-1>", lambda e: self.write_selected())

    def read_all(self):
        if self.app.bus is None or self.app.busy:
            return
        dxl_id = self.app.dxl_id
        self.app.run_bus(lambda bus: bus.read_block(dxl_id, 0, TABLE_END), self._show,
                         "Read Control Table")

    def _show(self, data):
        for reg in r.TABLE:
            raw = reg.decode(data[reg.address:])
            value = reg.scaled(raw)
            if reg is r.BAUD_RATE:
                value = f"{r.BAUD_RATES.get(raw, '?')} bps"
            elif reg is r.OPERATING_MODE:
                value = r.OPERATING_MODES.get(raw, "")
            elif reg is r.HARDWARE_ERROR_STATUS:
                value = r.hw_error_text(raw)
            hexa = f"0x{raw & ((1 << 8 * reg.size) - 1):0{2 * reg.size}x}"
            self.tree.set(reg.name, "raw", f"{raw}  ({hexa})")
            self.tree.set(reg.name, "value", "" if value == str(raw) else value)

    def _auto(self):
        if self._auto_job is not None:
            self.after_cancel(self._auto_job)
            self._auto_job = None
        if self.auto_var.get():
            self.read_all()
            self._auto_job = self.after(1000, self._auto)

    def write_selected(self):
        if not self.app.require_idle() or self.app.bus is None:
            return
        selection = self.tree.selection()
        if not selection:
            return
        reg = r.BY_NAME[selection[0]]
        if not reg.writable:
            messagebox.showinfo("Doppelstep", f"{reg.name} is read-only.")
            return
        value = simpledialog.askinteger(
            "Write register", f"{reg.name} (address {reg.address}, {reg.size} byte"
            f"{'s' if reg.size > 1 else ''}, {'signed' if reg.signed else 'unsigned'}):",
            parent=self)
        if value is None:
            return
        dxl_id = self.app.dxl_id

        def done(_status):
            self.app.log(f"{reg.name} = {value}")
            if reg is r.ID:
                self.app.id_var.set(value)
                self.app.log("ID changed: reconnect with the new ID.")
                self.app.disconnect()
            elif reg is r.BAUD_RATE:
                from .app import BAUD_CHOICES
                self.app.baud_var.set(BAUD_CHOICES[value])
                self.app.log("Baud Rate changed: reconnect at the new rate. MCUboot stays at "
                             "1 Mbps whatever this register says (docs/firmware-update.md).")
                self.app.disconnect()
            else:
                self.read_all()

        def work(bus):
            try:
                return bus.write(dxl_id, reg, value)
            except p.DxlError as exc:
                raise RuntimeError(f"{reg.name} = {value} refused: "
                                   f"{p.ERROR_NAMES.get(exc.error, exc.error)}") from exc

        self.app.run_bus(work, done, f"Write {reg.name}")

    def on_connection(self, connected):
        self._update()

    def on_busy(self, name):
        self._update()

    def _update(self):
        state = "normal" if self.app.bus is not None and not self.app.busy else "disabled"
        self.read_button.configure(state=state)
        self.write_button.configure(state=state)
