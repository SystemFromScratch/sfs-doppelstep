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
| Zephyr: Build | `west build --sysbuild -b b_g431b_esc1 -s app -d build`: MCUboot + signed application ([Firmware update](firmware-update.md)) | same, in Docker |
| Zephyr: Pristine Build | `west build -p always --sysbuild …` | same, in Docker |
| Zephyr: Pristine Build (simulated motor) | `west build -p always --sysbuild … -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay` | same, in Docker |
| Zephyr: Build (native_sim) | `west build -b native_sim/native/64 … -d build-native` | same, in Docker |
| Zephyr: Run tests (native_sim) | `scripts/run-tests.sh`: SIL and integration tests → `build-tests/junit.xml` | same, in Docker |
| Zephyr: Run (native_sim) | `tools/sim/run_native.py run` (bus at `build-native/ttyDXL`) | same, in Docker; bus also on TCP 5555 of the host |
| Zephyr: Clean | removes `build/` | removes `build/` |
| Zephyr: Flash (OpenOCD, ST-LINK) | `west flash -r openocd`: MCUboot and the signed application | `openocd … program build/mcuboot/zephyr/zephyr.hex`, then `build/app/zephyr/zephyr.signed.hex` (OpenOCD on Windows) |
| Python: Create .venv | `.venv` (or `.venv-linux`) with `requirements.txt` | `.venv` with `requirements.txt` |
| GUI: Run | `tools/gui/doppelstep_gui.py` | same |
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

Both build the stage `tools` of the `Dockerfile`: the tools only, the workspace stays in the
volume. A J-Link archive in `docker/jlink/` goes into this local image only.

## CI and the environment image

The stage `ci` of the `Dockerfile` adds the west workspace of `west.yml` (Zephyr, the modules,
MCUboot) in `/workdir`. GitHub Actions publishes it to the GitHub Container Registry as
`ghcr.io/systemfromscratch/sfs-doppelstep-env:<hash>`, where `<hash>` is taken from the files
the image is made from: `Dockerfile`, `.dockerignore`, `west.yml`, `scripts/west-init.sh`,
`tests/requirements.txt`.

| Event | Image with that hash in ghcr.io | Missing |
|---|---|---|
| Push to `main` | pulled; `:main` points at it | built, published as `:<hash>` and `:main` |
| Pull request, other branch | pulled | built in the job for that run, not published |

The firmware job then runs `scripts/ci.sh` in the image, with the repository mounted at
`/workdir/doppelstep` and `WEST_UPDATE=0`: the workspace is the one the image was built with, so
the projects that follow `main` (X430 emulator, Stepper-Lab, power-spice) change only with a new
image, that is with a change of one of the files above. The repository is private, and so is the
package; the workflow reads and writes it with `GITHUB_TOKEN`. The image never contains the
J-Link Software (not redistributable): `docker/jlink/` holds no archive in a checkout.

## Codespaces

**Code → Codespaces → … → New with options**, configuration
**Doppelstep (Codespaces: build and native_sim tests)** (`.devcontainer/codespaces/`). It starts
from `ghcr.io/systemfromscratch/sfs-doppelstep-env:main` on 4 cores; nothing is downloaded or
built at the start. `setup.sh` makes `/workspaces` the west topdir: links to the workspace of the
image and a west configuration that points at the clone. When `west.yml` of the branch differs
from the one of the image it says so; run `west update --narrow --fetch-opt=--tags` then.

In a codespace:

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build   # board images
bash scripts/run-tests.sh                                          # SIL and integration tests
bash scripts/ci.sh                                                 # everything, as CI
```

Flashing, debugging and the GUI need the board and a display: they stay local. If a codespace
cannot pull the image, give the repository access to the package: package settings on GitHub,
**Manage Codespaces access**.

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
