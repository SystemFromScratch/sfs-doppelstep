# GUI

!!! note "Planned"
    The Doppelstep GUI is not written yet. The repository, the VS Code tasks
    and this documentation chapter are prepared for it.

The GUI is a host-side desktop tool that talks to the firmware over the
DYNAMIXEL Protocol 2.0 bus (ST-LINK Virtual COM Port, 1 Mbps): ping, read and
write the Control Table, move the joint and watch the live registers.

## Contract

| Item | Contract |
|---|---|
| Entry point | `tools/gui/doppelstep_gui.py` |
| Simulator | `--sim`: runs without hardware |
| Dependencies | Python 3.10+, Tkinter, root `requirements.txt` (`pyserial`) |
| Tests | `tools/gui/tests/` (pytest), host-only |

## Starting the GUI

| Platform | VS Code task | Script | Virtual environment |
|---|---|---|---|
| Windows host | **GUI: Run**, **GUI: Run (simulator)** | `.vscode/host-menu.ps1` | `.venv` |
| Linux desktop | same tasks | `.vscode/workspace-menu.sh` | `.venv` (`.venv-linux` next to a Windows `.venv`) |

The virtual environment is created on first use. Tkinter is part of Python:
on Windows it comes with the python.org installer ("tcl/tk and IDLE"), on
Debian/Ubuntu install `python3-tk` and `python3-venv`.

The GUI needs a display, so it runs on the host and not inside the Dev
Container.
