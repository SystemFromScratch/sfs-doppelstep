# SPDX-License-Identifier: Apache-2.0
"""Test scenarios tab: choose, run, read the checks, save the HTML report."""

import os
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

from .. import report, scenarios
from . import style

GROUPS = (("boot", "Startup (tests/sil/test_boot.py)"),
          ("bus", "DYNAMIXEL bus (tests/sil/test_bus.py)"),
          ("eeprom", "EEPROM in flash (tests/sil/test_eeprom.py)"),
          ("motion", "Motion (tests/sil/test_motion.py)"))
CHECKED, UNCHECKED = "☑", "☐"


class TestsTab(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent, padding=8)
        self.app = app
        self.results = []
        self.stop_event = threading.Event()
        self.selected = {s.id for s in scenarios.SCENARIOS}

        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True)
        paned.add(self._build_list(paned), weight=2)
        paned.add(self._build_results(paned), weight=3)

    # --- scenario list -------------------------------------------------------------------

    def _build_list(self, parent):
        frame = ttk.Frame(parent)
        bar = ttk.Frame(frame)
        bar.pack(fill=tk.X, pady=(0, 6))
        self.run_button = ttk.Button(bar, text="▶ Run selected", style="Accent.TButton",
                                     command=self.run_selected)
        self.run_button.pack(side=tk.LEFT)
        self.stop_button = ttk.Button(bar, text="■ Stop", command=self.stop_event.set,
                                      state="disabled")
        self.stop_button.pack(side=tk.LEFT, padx=4)
        ttk.Button(bar, text="All", width=5, command=lambda: self._select(True)).pack(
            side=tk.LEFT, padx=(12, 2))
        ttk.Button(bar, text="None", width=5, command=lambda: self._select(False)).pack(
            side=tk.LEFT)

        self.tree = ttk.Treeview(frame, columns=("flags", "result"), show="tree headings",
                                 selectmode="browse")
        self.tree.heading("#0", text="Scenario")
        self.tree.heading("flags", text="Uses")
        self.tree.heading("result", text="Result")
        self.tree.column("#0", width=340)
        self.tree.column("flags", width=110, anchor="w")
        self.tree.column("result", width=80, anchor="center")
        for status, colour in style.RESULT_COLOURS.items():
            self.tree.tag_configure(status, foreground=colour)
        for group, title in GROUPS:
            self.tree.insert("", tk.END, iid=group, text=title, open=True)
        for sc in scenarios.SCENARIOS:
            flags = " + ".join(x for x, on in (("motor", sc.moves), ("EEPROM", sc.eeprom)) if on)
            self.tree.insert(sc.id.split("-")[0], tk.END, iid=sc.id,
                             text=f"{CHECKED} {sc.title}", values=(flags, ""))
        self.tree.pack(fill=tk.BOTH, expand=True)
        self.tree.bind("<Button-1>", self._on_click)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._show_details())

        self.progress = ttk.Progressbar(frame, mode="determinate")
        self.progress.pack(fill=tk.X, pady=(6, 0))
        self.summary = ttk.Label(frame, text="Not run yet.", style="Heading.TLabel")
        self.summary.pack(anchor="w", pady=(4, 0))
        ttk.Label(frame, text="Every scenario starts with a DYNAMIXEL Reboot (the MCU resets) "
                              "and ends with the torque off. The suite expects the factory "
                              "EEPROM: ID 1 at 1 Mbps.", style="Muted.TLabel",
                  wraplength=520).pack(anchor="w", pady=(4, 0))
        return frame

    def _on_click(self, event):
        item = self.tree.identify_row(event.y)
        if not item or self.app.busy:
            return
        if self.tree.identify_column(event.x) != "#0":
            return
        if item in dict(GROUPS):
            ids = self.tree.get_children(item)
            self._toggle(ids, not all(i in self.selected for i in ids))
        else:
            self._toggle([item], item not in self.selected)

    def _toggle(self, ids, on):
        for sid in ids:
            (self.selected.add if on else self.selected.discard)(sid)
            title = scenarios_by_id()[sid].title
            self.tree.item(sid, text=f"{CHECKED if on else UNCHECKED} {title}")

    def _select(self, on):
        self._toggle([s.id for s in scenarios.SCENARIOS], on)

    # --- results -------------------------------------------------------------------------

    def _build_results(self, parent):
        frame = ttk.Frame(parent)
        bar = ttk.Frame(frame)
        bar.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(bar, text="Checks of the selected scenario", style="Heading.TLabel").pack(
            side=tk.LEFT)
        self.report_button = ttk.Button(bar, text="Save HTML report …", command=self.save_report,
                                        state="disabled")
        self.report_button.pack(side=tk.RIGHT)

        self.details = tk.Text(frame, height=8, wrap="word", relief="flat",
                               background="#f6f7f9", font=("Segoe UI", 9))
        self.details.pack(fill=tk.X)
        self.details.configure(state="disabled")

        self.checks = ttk.Treeview(frame, columns=("measured", "expected", "result"),
                                   show="tree headings")
        self.checks.heading("#0", text="Check")
        self.checks.heading("measured", text="Measured")
        self.checks.heading("expected", text="Expected")
        self.checks.heading("result", text="Result")
        self.checks.column("#0", width=300)
        self.checks.column("measured", width=150)
        self.checks.column("expected", width=170)
        self.checks.column("result", width=70, anchor="center")
        for status, colour in style.RESULT_COLOURS.items():
            self.checks.tag_configure(status, foreground=colour)
        self.checks.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        return frame

    def _show_details(self):
        selection = self.tree.selection()
        if not selection or selection[0] in dict(GROUPS):
            return
        sc = scenarios_by_id()[selection[0]]
        result = next((x for x in self.results if x.scenario.id == sc.id), None)
        lines = [f"SIL test: {sc.source}"]
        if result is not None and result.error:
            lines.insert(0, f"{result.status}: {result.error}")
        if sc.board_note:
            lines.append(f"Board: {sc.board_note}")
        if sc.omitted:
            lines.append(f"Not checked on the board: {sc.omitted}")
        if sc.eeprom:
            lines.append(f"EEPROM changed and restored: {sc.eeprom}")
        if result is not None:
            lines += [f"Note — {c.name}: {c.note}" for c in result.checks if c.note]
        self.details.configure(state="normal")
        self.details.delete("1.0", tk.END)
        self.details.insert(tk.END, "\n".join(lines))
        self.details.configure(state="disabled")

        self.checks.delete(*self.checks.get_children())
        if result is None:
            return
        for check in result.checks:
            status = "INFO" if check.ok is None else ("PASS" if check.ok else "FAIL")
            self.checks.insert("", tk.END, text=check.name + (" ⓘ" if check.note else ""),
                               tags=(status,), values=(check.measured, check.expected, status))

    # --- running -------------------------------------------------------------------------

    def run_selected(self):
        if not self.app.require_idle():
            return
        if self.app.bus is None:
            messagebox.showwarning("Doppelstep", "Connect to the device first.")
            return
        chosen = [s for s in scenarios.SCENARIOS if s.id in self.selected]
        if not chosen:
            messagebox.showinfo("Doppelstep", "Select at least one scenario.")
            return
        if self.app.dxl_id != scenarios.DXL_ID or self.app.baudrate != 1000000:
            messagebox.showwarning(
                "Doppelstep", "The scenarios repeat the SIL tests, which start from the factory "
                "EEPROM: connect to ID 1 at 1 000 000 bps.")
            return
        warnings = []
        if any(s.moves for s in chosen):
            warnings.append("• The motor will turn. Check that the shaft can rotate freely and "
                            "that nothing is attached that can be damaged.")
        if any(s.eeprom for s in chosen):
            warnings.append("• Some scenarios change EEPROM registers and put the factory "
                            "values back. Factory Reset sets ALL EEPROM registers to their "
                            "factory values.")
        warnings.append("• Every scenario sends a DYNAMIXEL Reboot: the MCU resets.")
        if not messagebox.askokcancel("Run test scenarios",
                                      f"{len(chosen)} scenario(s) will run.\n\n"
                                      + "\n\n".join(warnings)):
            return

        self.results = []
        for sc in scenarios.SCENARIOS:
            self.tree.set(sc.id, "result", "" if sc in chosen else "—")
            self.tree.item(sc.id, tags=())
        self.progress.configure(maximum=len(chosen), value=0)
        self.summary.configure(text=f"Running 0 / {len(chosen)} …", style="Heading.TLabel")
        self.stop_event.clear()
        self.stop_button.configure(state="normal")
        self.app.set_busy("test run")
        self.app.log(f"Test run: {len(chosen)} scenarios")
        bus = self.app.bus
        self._mark_running(chosen[0])

        def on_result(result):
            self.app.post(self._on_result, result, chosen)

        def worker():
            try:
                results = scenarios.run_scenarios(bus, chosen, self.stop_event,
                                                  log=self.app.log_from_thread,
                                                  on_result=on_result)
            except Exception as exc:  # noqa: BLE001
                self.app.post(self.app.log, f"Test run aborted: {exc!r}")
                results = self.results
            self.app.post(self._finished, results, len(chosen))

        threading.Thread(target=worker, daemon=True).start()

    def _mark_running(self, scenario):
        self.tree.set(scenario.id, "result", "running")
        self.tree.item(scenario.id, tags=("RUNNING",))
        self.tree.see(scenario.id)

    def _on_result(self, result, chosen):
        self.results.append(result)
        self.tree.set(result.scenario.id, "result", result.status)
        self.tree.item(result.scenario.id, tags=(result.status,))
        self.progress.configure(value=len(self.results))
        self.summary.configure(text=f"Running {len(self.results)} / {len(chosen)} …")
        index = chosen.index(result.scenario)
        if index + 1 < len(chosen) and not self.stop_event.is_set():
            self._mark_running(chosen[index + 1])
        self.tree.selection_set(result.scenario.id)

    def _finished(self, results, total):
        self.app.set_busy(None)
        self.stop_button.configure(state="disabled")
        self.results = list(results)
        for sc in scenarios.SCENARIOS:
            if self.tree.set(sc.id, "result") == "running":
                self.tree.set(sc.id, "result", "")
                self.tree.item(sc.id, tags=())
        passed = sum(1 for x in results if x.status == scenarios.PASS)
        failed = len(results) - passed
        not_run = total - len(results)
        text = f"PASS {passed}   FAIL/ERROR {failed}" + (f"   not run {not_run}" if not_run else "")
        ok = failed == 0 and not_run == 0 and results
        self.summary.configure(text=("✔ " if ok else "✘ ") + text,
                               style="Good.TLabel" if ok else "Bad.TLabel")
        self.report_button.configure(state="normal" if results else "disabled")
        self.app.log(f"Test run finished: {text}")

    # --- report --------------------------------------------------------------------------

    def save_report(self):
        default = time.strftime("doppelstep-report-%Y%m%d-%H%M%S.html")
        path = filedialog.asksaveasfilename(title="Save the test report", initialfile=default,
                                            defaultextension=".html",
                                            filetypes=[("HTML", "*.html")])
        if not path:
            return
        report.save(path, report.build(self.results, self.app.connection_info(),
                                       self.app.last_update))
        self.app.log(f"Report saved: {path}")
        if messagebox.askyesno("Doppelstep", "Report saved. Open it in the browser?"):
            webbrowser.open("file://" + os.path.abspath(path))

    def on_connection(self, connected):
        self._update()

    def on_busy(self, name):
        self._update()

    def _update(self):
        idle = not self.app.busy
        self.run_button.configure(state="normal" if idle and self.app.bus is not None
                                  else "disabled")


_BY_ID = None


def scenarios_by_id():
    global _BY_ID
    if _BY_ID is None:
        _BY_ID = {s.id: s for s in scenarios.SCENARIOS}
    return _BY_ID
