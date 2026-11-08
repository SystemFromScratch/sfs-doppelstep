# Doppelstep GUI

Host-side desktop tool for Doppelstep on the B-G431B-ESC1: talks to the firmware over the
DYNAMIXEL Protocol 2.0 bus (ST-LINK Virtual COM Port) for motion and live plots, the Control
Table, the test scenarios (register checks of the SIL tests, HTML report) and the firmware update
through MCUboot serial recovery. Manual: [docs/gui/index.md](../../docs/gui/index.md).

| Item | Contract |
|---|---|
| Entry point | `tools/gui/doppelstep_gui.py [--port COM5] [--baud 1000000] [--id 1]` |
| Start from | any directory (own files are resolved relative to `__file__`) |
| Target | the board only; no simulator mode |
| Dependencies | Python 3.11+, Tkinter, root `requirements.txt` (`pyserial`, `matplotlib`, `smpmgr` → `smpclient`) |
| Tests | `tools/gui/tests/` (pytest), host-only, run by `scripts/ci.sh` |
| Documentation | `docs/gui/index.md` (part of the HTML site and the PDF manual) |

Start it with the VS Code task **GUI: Run** — on Windows through `.vscode/host-menu.ps1`
(`.venv`), on Linux through `.vscode/workspace-menu.sh` (`.venv`, or `.venv-linux` next to a
Windows `.venv`). Both create the virtual environment on first use.

By hand:

```powershell
# Windows
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tools\gui\doppelstep_gui.py
```

```bash
# Linux (Debian/Ubuntu: apt install python3-venv python3-tk)
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/gui/doppelstep_gui.py
```

Tests:

```bash
python -m pytest tools/gui/tests
```

The GUI is a desktop application: run it on the host (Windows or a Linux desktop), not inside the
Dev Container, which has no display.
