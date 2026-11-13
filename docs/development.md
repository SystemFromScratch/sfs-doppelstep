# Development environment

Doppelstep keeps the whole toolchain in a Docker image, so a build gives the same result on
Windows, on Linux, in CI and in GitHub Codespaces. This page describes the environment:
the repository, the VS Code tasks, the Dev Container and the debug probes, CI and its image,
and the documentation tooling.

## Requirements

| Platform | Install |
|---|---|
| Windows | Docker Desktop (WSL 2 backend), Git, VS Code with the Dev Containers extension. For debugging on the host: OpenOCD 0.12+ (for example xPack) with the ST-LINK driver, and/or the SEGGER J-Link Software |
| Linux | Docker and Git, or a native Zephyr installation with `west`; VS Code optional |
| Browser only | A GitHub account: [Codespaces](#codespaces) |

## Repository layout

```text
doppelstep/                             west manifest repository and Zephyr module
├── west.yml                            Zephyr v4.5.0, Stepper-C v1.3.0, X430 emulator (commit), DXL2-C v1.0.0,
│                                       Stepper-Lab and sfs-stepper-power-spice (commits)
├── zephyr/module.yml                   the repository is a Zephyr module: board, bindings, drivers
├── CMakeLists.txt, Kconfig             build modules/ and drivers/
├── app/                                Zephyr application (west build -s app)
│   ├── Kconfig, Kconfig.app_msg        threads, queues, startup
│   ├── prj.conf                        options common to every board
│   ├── sim.overlay                     simulated motor on the board (EXTRA_DTC_OVERLAY_FILE)
│   ├── sim-test.conf                   log period of the model for the tests
│   ├── boards/b_g431b_esc1.overlay     power stage, motor, servo: the hardware of this board
│   ├── boards/b_g431b_esc1.conf        FPU, zero-latency IRQ, OPAMP, RTT
│   ├── boards/native_sim_native_64.*   simulated stage, servo on uart1 (PTY), 1 ms tick, stdout logs
│   ├── sysbuild.conf, sysbuild/        MCUboot: one slot, signature, serial recovery
│   ├── VERSION                         firmware version (log, MCUboot image header)
│   └── src/
│       ├── main.c                      startup, STATUS LED
│       ├── control_task.c              slow loop, commands, publication of the status
│       ├── comm_task.c                 DYNAMIXEL bus
│       ├── joint_bridge.[ch]           X430 registers <-> motor commands
│       ├── joint.[ch]                  microsteps <-> DYNAMIXEL units, profiles
│       ├── app_msg.[ch], app_tasks.h   queues and task ids
│       └── sim_log.c                   periodic log line of the simulated motor
├── drivers/
│   ├── power_stage/                    STM32 and simulation backends, model adapter
│   ├── motor/                          Stepper-C behind the classes stepper and stepper_ctrl
│   └── x430/                           X430 emulator on a UART, NVS
├── include/doppelstep/drivers/         public driver APIs
├── dts/bindings/                       sfs,* bindings, vendor prefix sfs
├── boards/st/b_g431b_esc1/             out-of-tree board (not in Zephyr 4.5)
│   ├── b_g431b_esc1.dts                pinout from UM2516 Table 4, 170 MHz from 8 MHz HSE, partitions
│   ├── board.cmake                     runners: openocd (default), jlink, stm32cubeprogrammer, pyocd
│   └── support/openocd.cfg             ST-LINK/V2-1, STM32G4
├── modules/                            build glue of the libraries
│   ├── stepper-c/                      Kconfig (CONFIG_SSTEP_*), CMakeLists.txt
│   ├── dynamixel-x430/                 Kconfig (sources Kconfig.x430), CMakeLists.txt
│   └── stepper-lab/                    plant model sstep_plant.c, params/*.json -> sstep_plant_params.h
├── tests/
│   ├── app/, drivers/                  ztest suites, run by Twister
│   └── sil/, integration/              pytest against the native_sim firmware
├── tools/
│   ├── gui/                            host GUI: test bench and firmware update
│   ├── sim/                            native_sim launcher (PTY link, TCP bridge), Protocol 2.0 smoke test
│   └── docs/                           PDF manual builder and styles, MkDocs hooks, diagrams
├── docs/, overrides/, mkdocs.yml       this documentation (MkDocs Material)
├── scripts/
│   ├── west-init.sh                    west init -l + west update (in the container)
│   ├── run-tests.sh / ci.sh            tests on native_sim / the whole of CI
│   ├── check-layers.sh                 dependency rules between the layers
│   ├── junit_summary.py                test table for the GitHub job summary
│   ├── package_release.py              release checks and release files
│   └── host/                           GDB servers on the host (OpenOCD, J-Link; Windows and Linux)
├── .github/workflows/                  ci.yml (image, builds, tests), docs.yml (website, PDF),
│                                       release.yml (GitHub Release on a vX.Y.Z tag)
├── CHANGELOG.md                        changes of every release
├── .devcontainer/                      Dev Container configurations (host debug server, USB probe, Codespaces)
├── .vscode/                            tasks for Windows and Linux, Cortex-Debug launch configurations
├── .agents/skills/doppelstep/          engineering rules for agents
├── docker/jlink/                       optional J-Link Software archive for the image
├── Dockerfile, compose.yaml            Zephyr toolchain image
├── requirements.txt                    host Python tools: GUI, firmware update, documentation
└── LICENSE (Apache-2.0)
```

The west workspace lives in the container under `/workdir` (the named Docker volume
`doppelstep-west`): `doppelstep/` is a bind mount of this repository, next to it are
`zephyr/` and `modules/` (the HAL, CMSIS, SEGGER, MCUboot and its crypto modules, and the
libraries of `west.yml` under `modules/lib/`). Zephyr and the modules stay in the Linux
volume (fast file access, kept across container rebuilds); only this repository and the
build directories live in the host folder.

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
| Docs: Build PDF | `tools/docs/build_manual.py` → `site/Doppelstep.pdf` | same (installs Pango in the container when missing) |

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

### Mode A: USB probe in the container (usbipd-win)

```powershell
winget install usbipd                 # once
usbipd list                           # find the ST-LINK / J-Link BUSID
usbipd bind --busid <BUSID>           # once per device (administrator)
usbipd attach --wsl --busid <BUSID>   # after every replug, before the container starts
```

The container runs privileged with `/dev/bus/usb` mounted; `lsusb` must show the probe. For
the J-Link runner put `JLink_Linux_V*_x86_64.tgz` into `docker/jlink/` before the image is
built (`docker/jlink/README.md`).

### Mode B: GDB server on the host

```powershell
.\scripts\host\openocd-server.ps1          # ST-LINK, original firmware   -> :3333
.\scripts\host\jlink-gdb-server.ps1        # J-Link OB / J-Link           -> :2331
```

On a Linux or macOS host the same servers start with `bash scripts/host/openocd-server.sh` and
`bash scripts/host/jlink-gdb-server.sh` (OpenOCD 0.12+ or the J-Link Software installed on the
host; the port is set by `GDB_PORT` for OpenOCD and by `PORT` for J-Link).

Both listen on all interfaces: the container reaches the host as `host.docker.internal`
(Windows Firewall may ask once).

### Without VS Code

```bash
docker compose build
docker compose run --rm zephyr bash scripts/west-init.sh
docker compose run --rm zephyr                       # west build for the board
docker compose --profile usb run --rm zephyr-usb     # shell with the USB probe
```

## Flashing and debugging

### `west` (mode A, or the J-Link Remote Server)

```bash
west flash        -d build -r openocd     # ST-LINK/V2-1 (default runner)
west debug        -d build -r openocd
west debugserver  -d build -r openocd     # GDB server on :3333
west flash        -d build -r jlink       # J-Link OB / J-Link
west debug        -d build -r jlink
west debugserver  -d build -r jlink       # GDB server on :2331
west attach       -d build -r openocd|jlink
```

The openocd runner uses `boards/st/b_g431b_esc1/support/openocd.cfg` and the OpenOCD of the
Zephyr SDK (Zephyr thread awareness). In mode B the J-Link runner in the container can still
reach the probe through `scripts/host/jlink-remote-server.ps1` (`--dev-id <host IP>
--dev-id-type ip`); this path is not yet verified with the J-Link OB firmware.

### VS Code (Cortex-Debug, `.vscode/launch.json`)

| Configuration | Server |
|---|---|
| Debug: OpenOCD in container (ST-LINK, USB pass-through) | OpenOCD of the SDK in the container, `rtos Zephyr` |
| Debug: J-Link in container (J-Link OB, USB pass-through) | `JLinkGDBServerCLExe` in the container |
| Debug: OpenOCD on host (host.docker.internal:3333) | `scripts/host/openocd-server.ps1` |
| Debug: J-Link GDB Server on host (host.docker.internal:2331) | `scripts/host/jlink-gdb-server.ps1` |
| Attach: … on host (no download) | Attach to the running firmware |

Every *Debug* configuration builds first, loads MCUboot and the signed application
(`zephyr.elf` alone would not boot: MCUboot checks the signature), resets and stops at `main`
of the application.

### Logs (RTT)

USART2 carries the DYNAMIXEL bus, so the console and the log use SEGGER RTT:

```bash
west rtt -d build -r openocd      # or -r jlink
```

With a host GDB server: `openocd-server.ps1 -Rtt` (TCP 9090), or the RTT telnet port 19021 of
the J-Link GDB Server.

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
`/workdir/doppelstep` and `WEST_UPDATE=0`: the workspace is the one the image was built with. Every project in
`west.yml` is pinned to a tag or a commit, so the workspace changes only with `west.yml`, and with
it the image tag. The workflow writes the package with
`GITHUB_TOKEN`. The package must be **public** (package settings on GitHub, **Change
visibility**), so that anyone's codespace, and `docker pull` without a login, can use it. The
image never contains the J-Link Software (not redistributable): `docker/jlink/` holds no
archive in a checkout.

The workflow `.github/workflows/docs.yml` builds the website and the PDF manual on every pull
request and publishes both on GitHub Pages from `main`
([systemfromscratch.github.io/sfs-doppelstep](https://systemfromscratch.github.io/sfs-doppelstep/),
PDF at `Doppelstep.pdf` there). It needs one repository setting: **Settings → Pages → Source:
GitHub Actions**.

## Releasing

A release is a `vX.Y.Z` tag on `main`. The version follows
[Semantic Versioning](https://semver.org/) and lives in two places that must agree: the tag
and `app/VERSION`, which Zephyr turns into `APP_VERSION_STRING` (logged at boot) and into the
version of the signed image that MCUboot checks. The release workflow refuses a tag that does
not match.

1. Set `app/VERSION` to the new version and move the entries of `## [Unreleased]` in
   `CHANGELOG.md` to `## [X.Y.Z] - YYYY-MM-DD`; update the compare links at its end.
2. Pin every project of `west.yml` to a tag or a full commit hash: a release must build the
   same code every time.
3. Re-measure the figures in the README and the documentation that changed (flash, RAM, test
   counts: the build log of CI) and update the project status.
4. Check locally, then in the container run what CI runs:

   ```bash
   python3 scripts/package_release.py --version X.Y.Z --check-only
   docker compose run --rm zephyr bash scripts/ci.sh
   docker compose run --rm zephyr python3 scripts/package_release.py --version X.Y.Z
   ```

   The last command writes the release files to `dist/`. **Actions → Release → Run
   workflow** does the same in GitHub Actions without publishing anything (artifact
   `release`).
5. Merge to `main`, then tag the merge commit and push the tag:

   ```bash
   git tag -a vX.Y.Z -m "Doppelstep X.Y.Z"
   git push origin vX.Y.Z
   ```

`.github/workflows/release.yml` then checks the tag, runs the whole CI workflow on the tagged
commit, builds the PDF manual with the version on its cover and publishes the GitHub Release.
Its notes are the CHANGELOG section of the version, and its files are the board images under
release names, MCUboot and the application merged into one HEX for the first flashing, the
manual and `SHA256SUMS`. A `0.x` version is published as a pre-release. The release images are
signed with the public development key of MCUboot ([Firmware update](firmware-update.md)).

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
cannot pull the image, check that the package is public (see above).

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
python tools/docs/build_manual.py  # PDF manual: site/Doppelstep.pdf
```

`tools/docs/build_manual.py` converts the pages of `nav` to one HTML document and renders it
to A4 with WeasyPrint, in the layout of the SystemFromScratch® manuals (Stepper-C): a cover
with the hero illustration, an imprint, the contents with page numbers, numbered chapters
with numbered sections and figures, the About pages unnumbered, and the license with its
full text as Appendix A. Running header and footer, PDF bookmarks and metadata are added.
Its settings are in `extra.manual` of `mkdocs.yml`, the print styles in
`tools/docs/manual.css`. The version is the latest `vX.Y.Z` tag, otherwise `git describe`.
WeasyPrint needs Pango: on Linux `apt install libpango-1.0-0 libpangoft2-1.0-0` (the task
does it in the container), on Windows MSYS2 with `mingw-w64-x86_64-pango`, which the script
finds in `C:\msys64\mingw64\bin`.

The website uses the SystemFromScratch® style: `overrides/home.html` renders the landing
page from `extra.landing` in `mkdocs.yml` (hero, problem, what you get, measured figures,
portfolio banner), with the quick start of `docs/index.md` below it; `extra.css` holds the
company palette. The landing page (`docs/index.md` and `extra.landing`) is web only; its
quick start is covered by [Getting started](getting-started.md) in the PDF. The hero
illustration and the diagrams in `docs/assets/images/` (`hero-3d.svg`, `architecture.svg`,
`command-path.svg`, `test-setup.svg`, `motor-wiring.svg`) are generated by
`python tools/docs/diagrams.py` from the structure of the code; change the script, never the
SVG, and check the labels against the code after a change. The GUI screenshots in
`docs/assets/images/gui/` show the GUI connected to the `native_sim` firmware
(`socket://localhost:5555`); retake them when the GUI changes.

Every number on the landing page and in the [Introduction](overview.md#key-properties) is
measured: the test counts from `build-ci/twister/twister.json` and the `junit.xml` files of
`scripts/ci.sh`, the sizes from the memory report of the board build. Update them when they
change.

New pages go into `docs/` and into `nav` of `mkdocs.yml`; the build is strict,
so broken links fail it.
