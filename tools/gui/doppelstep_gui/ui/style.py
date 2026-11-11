# SPDX-License-Identifier: Apache-2.0
"""ttk styles and colours of the GUI."""

import tkinter.font as tkfont
from tkinter import ttk

GOOD = "#0a7a32"
BAD = "#b3261e"
WARN = "#9a6700"
MUTED = "#5f6673"
CARD = "#eef1f5"
ACCENT = "#1f5fbf"
MONO = ("Consolas", 9)

RESULT_COLOURS = {"PASS": GOOD, "FAIL": BAD, "ERROR": BAD, "STOPPED": WARN, "INFO": WARN,
                  "RUNNING": ACCENT}


def apply(root):
    style = ttk.Style(root)
    for theme in ("vista", "clam"):
        if theme in style.theme_names():
            style.theme_use(theme)
            break
    base = tkfont.nametofont("TkDefaultFont")
    base.configure(size=10)
    bold = base.copy()
    bold.configure(weight="bold")
    big = base.copy()
    big.configure(size=12, weight="bold")
    root._fonts = (bold, big)               # keep references

    style.configure("Muted.TLabel", foreground=MUTED)
    style.configure("Good.TLabel", foreground=GOOD, font=bold)
    style.configure("Bad.TLabel", foreground=BAD, font=bold)
    style.configure("Warn.TLabel", foreground=WARN, font=bold)
    style.configure("Heading.TLabel", font=big)
    style.configure("Card.TFrame", background=CARD)
    style.configure("CardTitle.TLabel", background=CARD, foreground=MUTED)
    style.configure("Value.TLabel", background=CARD, font=bold)
    for name, colour in (("Good", GOOD), ("Bad", BAD), ("Warn", WARN)):
        style.configure(f"Card{name}.TLabel", background=CARD, foreground=colour, font=bold)
    style.configure("Accent.TButton", font=bold)
    style.configure("Stop.TButton", font=big, foreground=BAD)
    # Torque state in the Motion tab: unmistakable at a glance
    style.configure("TorqueOn.TLabel", background=GOOD, foreground="white", font=big,
                    padding=(10, 6), anchor="center")
    style.configure("TorqueOff.TLabel", background=CARD, foreground=MUTED, font=big,
                    padding=(10, 6), anchor="center")
    style.configure("TorqueActive.TButton", font=bold, foreground=GOOD)
    style.configure("Treeview", rowheight=22)
