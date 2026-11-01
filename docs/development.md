# Development environment

## Two platforms, one set of tasks

The VS Code tasks in `.vscode/tasks.json` work on Windows and on Linux. Each
task has a Linux command (Dev Container or Linux host) and a `windows`
override; both call a menu script with the same action name:

| Where VS Code runs | Script | Zephyr (`west`) | Debug probe | GUI and documentation |
|---|---|---|---|---|
| Dev Container (Linux) | `.vscode/workspace-menu.sh` | in the container | USB pass-through (mode A) or host GDB server (mode B) | `.venv` / `.venv-linux` |
| Linux host | `.vscode/workspace-menu.sh` | native `west`, or the Docker image when `west` is missing | native tools, or the `zephyr-usb` compose service | `.venv` |
| Windows host | `.vscode/host-menu.ps1` | Docker image (`docker compose run --rm zephyr`) | OpenOCD / J-Link installed on Windows (`scripts/host/`) | `.venv` |

On the Windows host, Zephyr runs in the same image and the same
`doppelstep-west` volume as the Dev Container, so both share the west
workspace, ccache and `build/`.

## Tasks

`Ctrl+Shift+B` opens **Workspace Menu**, which lists the actions of the
current platform. The other tasks run one action directly
(`Terminal → Run Task…`):

| Task | Linux / Dev Container | Windows host |
|---|---|---|
| Zephyr: West Init/Update | `scripts/west-init.sh` | same, in Docker |
| Zephyr: Build | `west build -b b_g431b_esc1 -s apps/doppelstep -d build` | same, in Docker |
| Zephyr: Pristine Build | `west build -p always …` | same, in Docker |
| Zephyr: Clean | removes `build/` | removes `build/` |
| Zephyr: Flash (OpenOCD, ST-LINK) | `west flash -r openocd` | `openocd … program build/zephyr/zephyr.elf` (OpenOCD on Windows) |
| Python: Create .venv | `.venv` (or `.venv-linux`) with `requirements.txt` | `.venv` with `requirements.txt` |
| GUI: Run / Run (simulator) | `tools/gui/doppelstep_gui.py [--sim]` | same |
| Docs: Serve | `mkdocs serve` on `http://127.0.0.1:8000` | same |
| Docs: Build PDF | `tools/docs/build_pdf.py` → `site/Doppelstep.pdf` | same (prints with Microsoft Edge) |

Actions only in the **Linux** menu: Menuconfig, Flash (J-Link),
Debug server (OpenOCD / J-Link), RTT log (OpenOCD / J-Link), Ccache stats/clear.

Actions only in the **Windows** menu: GDB server on the host (OpenOCD, with or
without RTT; J-Link) and the J-Link Remote Server — the servers that the
Cortex-Debug configurations "… on host" connect to from the Dev Container.

## Dev Container

`F1 → Dev Containers: Reopen in Container` and pick one of:

| Container | Debug probe |
|---|---|
| **Doppelstep (host debug server)** — default | stays on Windows; GDB connects to `host.docker.internal` (mode B) |
| **Doppelstep (USB probe in the container)** | attached to WSL 2 with usbipd-win (mode A) |

The first start runs `scripts/west-init.sh` (`west init -l`,
`west update --narrow --fetch-opt=--tags`) into the `doppelstep-west` volume.

## Python virtual environment

The GUI and the documentation use the packages in `requirements.txt`. The
tasks create the environment on first use; **Python: Create .venv** creates or
updates it explicitly.

- Windows: `.venv\Scripts\python.exe` (Python from python.org, with Tkinter).
- Linux: `.venv/bin/python`. In a folder bind-mounted from Windows where
  `.venv` already is a Windows environment, the Linux scripts use
  `.venv-linux` instead.

## Documentation and PDF manual

The documentation is written in Markdown in `docs/` and built with MkDocs
Material (`mkdocs.yml`):

```bash
mkdocs serve                       # live preview
mkdocs build                       # HTML site in site/
python tools/docs/build_pdf.py     # PDF manual: site/Doppelstep.pdf
```

`tools/docs/build_pdf.py` builds the site, prints the combined page of
`mkdocs-print-site-plugin` with Chromium (Playwright) to A4, fills in the page
numbers of the table of contents and adds a cover page
(`tools/docs/cover_page.tpl`), bookmarks and metadata. The browser is
Playwright's Chromium, Microsoft Edge or Google Chrome, whichever is found
first; on Linux the task installs Playwright's Chromium once. Print styles:
`docs/assets/stylesheets/print.css`.

New pages go into `docs/` and into `nav` of `mkdocs.yml`; the build is strict,
so broken links fail it.
