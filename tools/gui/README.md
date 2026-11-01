# Doppelstep GUI (planned)

Host-side desktop tool for Doppelstep: talks to the firmware over the
DYNAMIXEL Protocol 2.0 bus (ST-LINK Virtual COM Port, 1 Mbps) to ping, read
and write the Control Table, move the joint and watch the live registers.

The GUI is not written yet. This folder and the VS Code tasks are prepared for
it, so it only has to follow this contract:

| Item | Contract |
|---|---|
| Entry point | `tools/gui/doppelstep_gui.py` |
| Start from | any directory (resolve own files relative to `__file__`) |
| Simulator | `--sim`: runs without hardware (VS Code task **GUI: Run (simulator)**) |
| Dependencies | Python 3.10+, Tkinter, packages in the root `requirements.txt` (`pyserial`) |
| Tests | `tools/gui/tests/` (pytest), host-only |
| Documentation | `docs/gui/index.md` (part of the HTML site and the PDF manual) |

Start it with the VS Code tasks **GUI: Run** / **GUI: Run (simulator)** — on
Windows through `.vscode/host-menu.ps1` (`.venv`), on Linux through
`.vscode/workspace-menu.sh` (`.venv`, or `.venv-linux` next to a Windows
`.venv`). Both create the virtual environment on first use.

By hand:

```powershell
# Windows
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tools\gui\doppelstep_gui.py --sim
```

```bash
# Linux (Debian/Ubuntu: apt install python3-venv python3-tk)
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/gui/doppelstep_gui.py --sim
```

The GUI is a desktop application: run it on the host (Windows or a Linux
desktop), not inside the Dev Container, which has no display.
