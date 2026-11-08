# Doppelstep — Zephyr RTOS stepper motor controller on B-G431B-ESC1

> *Your stepper, seen as a smart servo.*
> Why the name: [docs/NAMING.md](docs/NAMING.md).

Zephyr RTOS firmware and development workspace for the STMicroelectronics
**B-G431B-ESC1** Discovery kit: a two-phase stepper motor (17HS19-2004S1) driven
in closed-loop microstepping current control by
[Stepper-C](https://github.com/AntonVynohradov/Stepper-C), seen by the host as a
**DYNAMIXEL XM430-W350** over Protocol 2.0 through the
[DYNAMIXEL X430 Control Table Emulator](https://github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator).
Target host: ROS 2 `dynamixel_hardware_interface` as used by
[OpenMANIPULATOR-X](https://github.com/ROBOTIS-GIT/open_manipulator).

The build result is MCUboot and the signed application (sysbuild, firmware update over
USART2: `docs/firmware-update.md`); flashing and debugging work with
OpenOCD (on-board ST-LINK/V2-1) and SEGGER J-Link (the ST-LINK converted to
J-Link OB, or an external J-Link), through `west` and VS Code Cortex-Debug.

The environment follows
[Zephyr-SmartFusion2-workspace](https://github.com/AntonVynohradov/Zephyr-SmartFusion2-workspace):
Docker image, VS Code Dev Container, pinned tool versions, `west` manifest in
this repository.

---

## Table of contents

- [Architecture](#architecture)
- [Repository structure](#repository-structure)
- [Tool and dependency versions](#tool-and-dependency-versions)
- [Hardware](#hardware)
- [Environment setup](#environment-setup)
- [VS Code tasks (Windows and Linux)](#vs-code-tasks-windows-and-linux)
- [Building](#building)
- [Flashing and debugging](#flashing-and-debugging)
- [Logs (RTT)](#logs-rtt)
- [DYNAMIXEL emulation](#dynamixel-emulation)
- [Simulated motor](#simulated-motor)
- [Tests and CI](#tests-and-ci)
- [Configuration](#configuration)
- [GUI](#gui)
- [Documentation and PDF manual](#documentation-and-pdf-manual)
- [Bring-up checklist](#bring-up-checklist)
- [Open items](#open-items)
- [Naming](#naming)
- [Licenses](#licenses)

---

## Architecture

```text
 ROS 2 / DynamixelSDK / DYNAMIXEL Wizard            host PC
        │ Protocol 2.0, 1 Mbps (ST-LINK Virtual COM Port for now)
 ───────┼─────────────────────────────────────────────────────── B-G431B-ESC1
        ▼ USART2 (PB3/PB4)
 ┌──────────────┐  callbacks  ┌─────────────┐ ──cmd_q──▶ ┌──────────────┐
 │ x430 driver  │ ──────────▶ │ comm_task + │ ◀──rsp_q── │ control_task │
 │ X430 emulator│             │ joint_bridge│ ◀─status_q─│ slow loop    │
 └──────────────┘             └─────────────┘            └──────┬───────┘
                                                                │ stepper, stepper_ctrl
                                                                ▼
                                                         ┌──────────────┐
                                                         │ motor driver │
                                                         │ Stepper-C    │
                                                         └──────┬───────┘
                                       fast loop in the zero-latency ADC interrupt, 20 kHz
                                                                ▼
                                                         ┌──────────────┐
                                                         │ power_stage  │ TIM1 + ADC1/ADC2 (LL)
                                                         │ STM32 backend│ OPAMP1..3
                                                         └──────┬───────┘
        L6387 gate drivers → 3 half-bridges → stepper motor     ▼
        phase A between U and W, phase B between V and W (shared-leg topology)
```

| Context | Rate | Work |
|---|---|---|
| ADC1_2 interrupt (zero latency, priority 0) | PWM rate, 20 kHz | read currents and VBUS, shared-leg (W) trip, `sstep_fast_loop()` |
| `control_task` (cooperative, −2) | 1 kHz, absolute schedule | `sstep_motor_process()`, commands on receipt, publication of the status |
| `comm_task` (preemptive, 2) | on RX + every 10 ms | `x430_servo_process()`, `joint_bridge`, live registers |
| `main` | — | startup, STATUS LED |

Layers, rules and the path of a command: [docs/architecture.md](docs/architecture.md). For another
MCU family: [docs/porting.md](docs/porting.md).

---

## Repository structure

```text
doppelstep/                             west manifest repository and Zephyr module (this repo)
├── west.yml                            Zephyr v4.5.0, Stepper-C v1.2.0, X430 emulator (main), DXL2-C v1.0.0,
│                                       Stepper-Lab and sfs-stepper-power-spice (main)
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
│   ├── b_g431b_esc1.dts                pinout from UM2516 Table 4, 170 MHz from 8 MHz HSE
│   ├── board.cmake                     runners: openocd (default), jlink, stm32cubeprogrammer, pyocd
│   └── support/openocd.cfg             ST-LINK/V2-1, STM32G4
├── modules/                            build glue of the libraries
│   ├── stepper-c/                      Kconfig (CONFIG_SSTEP_*), CMakeLists.txt
│   ├── dynamixel-x430/                 Kconfig (sources Kconfig.x430), CMakeLists.txt
│   └── stepper-lab/                    plant model sstep_plant.c, params/*.json -> sstep_plant_params.h
├── tools/
│   ├── gui/                            host GUI (planned, see tools/gui/README.md)
│   ├── sim/                            native_sim launcher (PTY link, TCP bridge), Protocol 2.0 smoke test
│   └── docs/                           PDF manual builder, MkDocs hooks, cover page
├── docs/                               MkDocs sources (architecture, porting, simulation, tests, ...)
├── tests/
│   ├── app/, drivers/                  ztest suites, run by Twister
│   └── sil/, integration/              pytest against the native_sim firmware
├── .github/workflows/ci.yml            layers, builds + tests in the Docker image (scripts/ci.sh)
├── scripts/
│   ├── west-init.sh                    west init -l + west update (in the container)
│   ├── run-tests.sh / ci.sh            tests on native_sim / the whole of CI
│   ├── check-layers.sh                 dependency rules between the layers
│   ├── junit_summary.py                test table for the GitHub job summary
│   └── host/                           GDB servers on the Windows host (OpenOCD, J-Link)
├── docker/jlink/                       optional J-Link Software archive for the image
├── .devcontainer/
│   ├── devcontainer.json               default: GDB server on the host (mode B)
│   └── usb-passthrough/devcontainer.json  probe in the container via usbipd (mode A)
├── .vscode/
│   ├── tasks.json                      tasks for Linux/Dev Container and the Windows host
│   ├── workspace-menu.sh               Linux / Dev Container actions
│   ├── host-menu.ps1                   Windows host actions
│   └── launch.json                     Cortex-Debug configurations
├── .agents/skills/doppelstep/          engineering rules for agents
├── mkdocs.yml, requirements.txt        documentation, PDF manual, host Python tools
├── Dockerfile, compose.yaml
└── LICENSE (Apache-2.0)
```

West workspace inside the container (topdir `/workdir`, a named Docker volume):

```text
/workdir/
├── .west/  .ccache/
├── doppelstep/                this repository (bind mount of the host folder)
├── zephyr/
└── modules/
    ├── hal/stm32  hal/cmsis_6  debug/segger
    └── lib/stepper-c  lib/dynamixel-x430  lib/dxl2-c  lib/stepper-lab  lib/sfs-stepper-power-spice
```

The repository folder on Windows may keep any name (for example
`C:\1.Projects\sfs-stepper-driver`); inside the container it is always
`/workdir/doppelstep`.

Zephyr and the modules stay in the Linux volume (fast file access, kept across
container rebuilds); only this repository and `build/` live in the Windows folder.

---

## Tool and dependency versions

| Item | Version | Where |
|---|---|---|
| Base image | `python:3.12.15-slim-bookworm` (digest pinned) | `Dockerfile` |
| west | 1.5.0 | `Dockerfile` |
| Zephyr SDK | 1.0.1 (minimal + `arm-zephyr-eabi` + host tools incl. OpenOCD) | `Dockerfile` |
| CMake | 3.30.5 (pip) | `Dockerfile` |
| PyYAML / pylink-square | 6.0.3 / 2.0.1 | `Dockerfile` |
| Zephyr | **v4.5.0** | `west.yml` |
| Stepper-C | **v1.2.0** | `west.yml` |
| DYNAMIXEL-X430 emulator | `main` (until its first release) | `west.yml` |
| DXL2-C | v1.0.0 (= `X430_DXL2_GIT_TAG` of the emulator) | `west.yml` |
| Stepper-Lab | `main` (until its first release): plant model and parameters of the simulated motor | `west.yml` |
| sfs-stepper-power-spice | `main` (until its first release): ngspice source of the stage parameters, not built | `west.yml` |
| Twister packages | anytree, junitparser, natsort, ply, tabulate | `Dockerfile` |
| Board | `b_g431b_esc1` (this repository) | `boards/` |

The X430 emulator normally downloads DXL2-C with CMake FetchContent; the module
glue points `FETCHCONTENT_SOURCE_DIR_DXL2` at the west project instead, so a
build needs no network access. It warns when the `dxl2-c` revision in
`west.yml` differs from the tag `x430.cmake` expects.

---

## Hardware

B-G431B-ESC1: STM32G431CBU6 (Cortex-M4F, 170 MHz, 128 KiB flash, 32 KiB SRAM),
L6387 gate drivers, STL180N6F7 MOSFETs, three 3 mΩ low-side shunts on the
internal OPAMPs, detachable ST-LINK/V2-1. Pinout: UM2516 Rev 4, Table 4
(see `boards/st/b_g431b_esc1/doc/index.rst`).

**Motor wiring (Stepper-C shared-leg topology):**

| Motor | ESC1 output |
|---|---|
| Phase A (coil A+ / A−) | U / W |
| Phase B (coil B+ / B−) | V / W |

W is shared and carries iA + iB; the firmware measures its shunt too and trips
on `|iW| > i_trip` (`aux-trip` of the motor node).

**Current and voltage scaling** come from the power-stage devicetree node:
3 mΩ shunts, gain −64/7 (≈ −9.14, OPAMP PGA 16 with VINM0 as bias input, as in
ST MCSDK, NuttX and SimpleFOC), VBUS divider 18 k / (18 k + 169 k). With a
3.3 V reference the current full scale is ±60.2 A (≈ 29 mA per ADC count) and
the VBUS full scale 34.3 V.

**DYNAMIXEL bus:** USART2 (PB3 TX / PB4 RX), currently through the ST-LINK
Virtual COM Port, 1 Mbps. A DYNAMIXEL X430-T has no direction pin either; for a
TTL half-duplex bus later, USART2 can run in single-wire mode (`single-wire;`
on `&usart2`, TX pin only).

---

## Environment setup

Requirements on Windows: Docker Desktop (WSL 2 backend), Git, VS Code with the
Dev Containers extension. For debugging on the host: OpenOCD 0.12+
(e.g. xPack) with the ST-LINK driver, and/or the SEGGER J-Link Software.

```powershell
git clone <repository-url> C:\1.Projects\doppelstep
code C:\1.Projects\doppelstep
```

`F1 → Dev Containers: Reopen in Container` and pick one of:

| Container | Debug probe | `west flash/debug/debugserver/rtt` in the container |
|---|---|---|
| **Doppelstep (host debug server)** — default | stays on Windows | GDB connects to the host server over TCP (mode B) |
| **Doppelstep (USB probe in the container)** | attached to WSL 2 with usbipd-win | yes, OpenOCD and J-Link (mode A) |

The first start runs `scripts/west-init.sh`: `west init -l` and
`west update --narrow --fetch-opt=--tags` into the `doppelstep-west`
volume (a few minutes). Run it again (task **Zephyr: West Init/Update**) after
changing `west.yml`.

Without the Dev Container, the VS Code tasks also work on the Windows host
(Zephyr in Docker, probe tools on Windows) and on a Linux host — see
[VS Code tasks](#vs-code-tasks-windows-and-linux).

### Mode A: USB probe in the container (usbipd-win)

```powershell
winget install usbipd                 # once
usbipd list                           # find the ST-LINK / J-Link BUSID
usbipd bind --busid <BUSID>           # once per device (administrator)
usbipd attach --wsl --busid <BUSID>   # after every replug, before the container starts
```

The container runs privileged with `/dev/bus/usb` mounted; `lsusb` must show
the probe. For the J-Link runner put `JLink_Linux_V*_x86_64.tgz` into
`docker/jlink/` before the image is built (see `docker/jlink/README.md`).

### Mode B: GDB server on the host

```powershell
.\scripts\host\openocd-server.ps1          # ST-LINK, original firmware   -> :3333
.\scripts\host\jlink-gdb-server.ps1        # J-Link OB / J-Link           -> :2331
```

Both listen on all interfaces (the container reaches the host as
`host.docker.internal`; Windows Firewall may ask once).

### Without VS Code

```bash
docker compose build
docker compose run --rm zephyr bash scripts/west-init.sh
docker compose run --rm zephyr                       # west build
docker compose --profile usb run --rm zephyr-usb     # shell with the USB probe
```

---

## VS Code tasks (Windows and Linux)

Every task in `.vscode/tasks.json` has a Linux command (Dev Container or Linux
host, `.vscode/workspace-menu.sh`) and a `windows` override (Windows host,
`.vscode/host-menu.ps1`); both scripts take the same action names.

| Where VS Code runs | Zephyr (`west`) | Debug probe | GUI, documentation |
|---|---|---|---|
| Dev Container | in the container | mode A or B | `.venv` / `.venv-linux` |
| Linux host | native `west`, or the Docker image when `west` is missing | native tools or `zephyr-usb` service | `.venv` |
| Windows host | Docker image (`docker compose run --rm zephyr`) | OpenOCD / J-Link on Windows (`scripts/host/`) | `.venv` |

| Task | What it does |
|---|---|
| **Workspace Menu** (`Ctrl+Shift+B`) | every action of the current platform |
| Zephyr: West Init/Update, Build, Pristine Build, Clean | west workspace and firmware (`build/`) |
| Zephyr: Flash (OpenOCD, ST-LINK) | Linux: `west flash -r openocd`; Windows: host OpenOCD |
| Python: Create .venv | virtual environment with `requirements.txt` |
| GUI: Run | `tools/gui/doppelstep_gui.py`: test bench and firmware update |
| Docs: Serve, Docs: Build PDF | `mkdocs serve`, `site/Doppelstep.pdf` |

Linux menu only: menuconfig, J-Link flash, debug servers, RTT logs, ccache.
Windows menu only: host GDB servers (OpenOCD with or without RTT, J-Link) and
the J-Link Remote Server. Details: [docs/development.md](docs/development.md).

---

## Building

```bash
west build --sysbuild -b b_g431b_esc1 -s app -d build              # incremental
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build    # pristine
west build -d build -t menuconfig                                  # Kconfig of the application
```

`Ctrl+Shift+B` opens the **Workspace Menu** (initialize/update, build,
pristine build, menuconfig, flash, RTT, GUI, documentation); see
[VS Code tasks](#vs-code-tasks-windows-and-linux).

Artifacts: `build/mcuboot/zephyr/zephyr.hex` (MCUboot, 37 KiB of its 38 KiB
partition) and in `build/app/zephyr/`: **`zephyr.elf`** (debugging),
`zephyr.signed.hex` (debug probe), `zephyr.signed.bin` (update over USART2).
Current size of the application: about 75 KiB flash of the 86 KiB slot (with the
simulated motor 85 KiB), 19 KiB RAM. Flash layout and update:
`docs/firmware-update.md`.

A pristine build is needed after changes to `prj.conf`, Kconfig, devicetree,
`west.yml` or the toolchain.

---

## Flashing and debugging

### `west` (mode A, or J-Link Remote Server)

```bash
west flash        -d build -r openocd     # ST-LINK/V2-1 (default runner)
west debug        -d build -r openocd
west debugserver  -d build -r openocd     # GDB server on :3333
west flash        -d build -r jlink       # J-Link OB / J-Link
west debug        -d build -r jlink
west debugserver  -d build -r jlink       # GDB server on :2331
west attach       -d build -r openocd|jlink
```

The openocd runner uses `boards/st/b_g431b_esc1/support/openocd.cfg` and the
OpenOCD of the Zephyr SDK (Zephyr thread awareness). In mode B the J-Link runner
in the container can still reach the probe through `scripts/host/jlink-remote-server.ps1`
(`--dev-id <host IP> --dev-id-type ip`); this path is not yet verified with the
J-Link OB firmware.

### VS Code (Cortex-Debug, `.vscode/launch.json`)

| Configuration | Server |
|---|---|
| Debug: OpenOCD in container (ST-LINK, USB pass-through) | OpenOCD of the SDK in the container, `rtos Zephyr` |
| Debug: J-Link in container (J-Link OB, USB pass-through) | `JLinkGDBServerCLExe` in the container |
| Debug: OpenOCD on host (host.docker.internal:3333) | `scripts/host/openocd-server.ps1` |
| Debug: J-Link GDB Server on host (host.docker.internal:2331) | `scripts/host/jlink-gdb-server.ps1` |
| Attach: … on host (no download) | attach to the running firmware |

Every *Debug* configuration builds first, loads MCUboot and the signed
application (`zephyr.elf` alone would not boot: MCUboot checks the signature),
resets and stops at `main` of the application.

---

## Logs (RTT)

USART2 carries the DYNAMIXEL bus, so the console and the log use SEGGER RTT:

```bash
west rtt -d build -r openocd      # or -r jlink
```

With a host GDB server: `openocd-server.ps1 -Rtt` (TCP 9090) or the J-Link GDB
Server's RTT telnet port 19021.

---

## DYNAMIXEL emulation

Model **XM430-W350** (Model Number 1020), one ID, Protocol 2.0. The emulator
validates every access (ranges, limits, EEPROM lock, Indirect Address, Fast
Sync/Bulk Read, Bus Watchdog …); the firmware maps the registers onto the motor driver (`app/src/joint_bridge.c`):

| Register | Mapping |
|---|---|
| Torque Enable (64) | `stepper_enable()` / `stepper_disable()`; refused (Result Fail) while the dead time is not set or with a fault |
| Operating Mode (11) | 3 position, 4 extended position, 1 velocity; 0, 5, 16 are refused (Data Range Error) |
| Goal Position (116) | `stepper_ctrl_move_to()`; limits from Profile Velocity/Acceleration (velocity- or time-based, Drive Mode bit 2), Velocity Limit and the `max-velocity` / `max-acceleration` properties of the motor node |
| Goal Velocity (104) | `stepper_ctrl_run()` or `stepper_ctrl_stop()` (velocity mode) |
| Drive Mode (10) bit 0, Homing Offset (20) | direction and offset of the position mapping |
| Current Limit (38) | run current = min(limit × 2.69 mA, `run-current-milliamp`) |
| Present Position (132), Position Trajectory (140) | commanded position (no encoder) |
| Present Velocity (128), Velocity Trajectory (136) | commanded velocity |
| Present Current (126) | phase current amplitude √(iA² + iB²) |
| Present Input Voltage (144) | measured VBUS (0.1 V) |
| Moving (122), Moving Status (123) | from the status of the motor driver |
| Hardware Error Status (70) | over/undervoltage → Input Voltage; overcurrent, W trip, HAL → Electrical Shock |
| Bus Watchdog | `stepper_ctrl_stop()` |
| REBOOT / FACTORY_RESET | outputs off, faults cleared; on the board (MCUboot) REBOOT then resets the MCU: silent for 500 ms plus startup, Present Position restarts (`docs/firmware-update.md`) |
| CLEAR (multi-turn) | position redefined within one turn |
| Baud Rate (8) | USART2 reconfigured after the status packet |

Unit conversions (`app/src/joint.c`): 4096 pulses per joint revolution,
0.229 rev/min, 214.577 rev/min², 2.69 mA; 3200 microsteps per motor revolution
(200 × 16), gear ratio `gear-motor-revs / gear-joint-revs` of the servo node (1/1).

**Startup position:** the motor has no encoder. At power-on the rotor position
is reported as Present Position **2048** (`start-position` of the servo node,
0 rad for `dynamixel_hardware_interface`): power the joint up in that position.
With torque off the rotor can be moved by hand and the reported position is
then no longer the real one.

**Factory values of this firmware:** `factory-id` (1) and `factory-baud-code`
(3 = 1 Mbps, the OpenMANIPULATOR-X bus speed) of the servo node. They apply while no EEPROM block is stored and after FACTORY_RESET; a
real XM430 leaves the factory at 57600 bps. ID and Baud Rate written over the
bus are stored in flash (NVS, the `storage` partition of the servo node); flash writes are refused
while the power stage is enabled (single-bank flash: an erase stalls the CPU).

**OpenMANIPULATOR-X:** its `ros2_control` description expects five
XM430-W350 on one bus (IDs 11–15, 1 Mbps, Operating Mode 3, Drive Mode 4 =
time-based profile). This firmware answers for **one** ID, so over the VCP the
host sees one joint; set `factory-id` to the joint's ID (for example
11). Fast Sync/Bulk Read answers come only from the IDs of one emulator bus.

---

## Simulated motor

For development without a motor, a `sfs,sim-power-stage` node puts a motor model
behind the power stage, so the whole chain ROS 2 → Protocol 2.0 → X430
emulator → Stepper-C runs as it will with the motor. The model is the plant
model of [Stepper-Lab](https://github.com/AntonVynohradov/Stepper-Lab)
(`sil/c/sstep_plant.c`, the code of its Xcos block) with its `params/*.json`;
the power stage values come from the ngspice model of
[sfs-stepper-power-spice](https://github.com/AntonVynohradov/sfs-stepper-power-spice).
Both are west projects (`main`). How the three repositories work together and
how to change the model or its parameters: [docs/simulation.md](docs/simulation.md).
Two ways:

| | A: on the board | B: on the PC (`native_sim`) |
|---|---|---|
| Hardware | B-G431B-ESC1, no motor needed | none |
| Timing | real: 20 kHz interrupt, UART 1 Mbps | simulated time, kept to real time |
| Bus | ST-LINK VCP (COM port) | pseudoterminal, or TCP 5555 from Docker |
| Debugging | J-Link / OpenOCD, RTT | gdb on `zephyr.exe`, logs on stdout |
| Use it for | interrupt load, priorities, final check | protocol and motion logic, ROS 2 integration, CI |

### A: on the board

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay
```

(task **Zephyr: Pristine Build (simulated motor)**; `sim.overlay` stays in the
CMake cache, so **Zephyr: Build** keeps the simulated motor until the next
**Zephyr: Pristine Build**.)

```text
 TIM1 TRGO2 → ADC → zero-latency interrupt, 20 kHz (real timing), real samples ignored
   sim_plant_sample()   ADC counts of the model (iA, iB, VBUS)
   sstep_fast_loop()    Stepper-C, unchanged
   sim_plant_step()     one PWM period of the model (RK4)
 compare values → model;  the backend never switches the real outputs: MOE stays 0
```

- **Model:** `sstep_plant.c` of Stepper-Lab in single precision with the
  averaged shared-leg stage plus the ngspice leg voltage error table
  (`CONFIG_SFS_SIM_LEG_ERROR`), one PWM period of computation delay as on the
  hardware. Parameters from Stepper-Lab `params/*.json`, each overridable
  with `CONFIG_SFS_SIM_*` (−1 = file value); load torque, locked rotor. The
  switching stage and the ngspice simulations stay offline (too heavy for
  50 µs); their results enter as parameters.
- **Safety:** the timer runs, but the simulation backend never calls the output enable of
  the real stage; the board may run with or without a motor and bus voltage. `dead-time-ns` does
  not have to be set.
- **What the host sees:** Present Current and Present Input Voltage come from
  the model. Present Position and Present Velocity are the commanded values,
  as with the real motor (no encoder).
- **RTT log** every `CONFIG_SFS_SIM_LOG_PERIOD_MS`: commanded and rotor
  position, lag in electrical degrees (above 90 the motor is close to losing
  steps; a multiple of 360 is 4 lost full steps), speed, phase currents and
  voltages, load, and the longest fast-loop callback in CPU cycles and µs.
- **Load torque** (`CONFIG_SFS_SIM_LOAD_UNM`, or `power_stage_sim_set_load()`) is a constant active torque, like gravity on a joint: it pulls the
  rotor back when the motor loses synchronism or the torque is off.

### B: on the PC (`native_sim`)

The same application built for `native_sim/native/64` is a Linux program
(`build-native/zephyr/zephyr.exe`, in the Docker image on Windows). The
overlay selects the simulation backend with the ESC1 values, and a 1 ms kernel timer runs the 20
fast-loop periods of each tick before the slow loop, as on the MCU. The
emulated EEPROM is kept in `build-native/flash.bin`.

```bash
west build -b native_sim/native/64 -s app -d build-native   # task "Zephyr: Build (native_sim)"
python3 tools/sim/run_native.py run [--tcp 5555]                       # task "Zephyr: Run (native_sim)"
python3 tools/sim/dxl_smoke_test.py build-native/ttyDXL                # ping, torque, move, without ROS 2
```

`run_native.py run` links the bus pseudoterminal to `build-native/ttyDXL`
(its `/dev/pts/N` changes at every start). In the Docker image (task on the
Windows host) it also serves the bus on TCP port 5555, published to the
host. Where the host program runs:

| Host program | Port to give it |
|---|---|
| same Linux machine / Dev Container | `build-native/ttyDXL` |
| WSL or another Linux machine (ROS 2) | `python3 tools/sim/run_native.py connect --host localhost --port 5555` → `/tmp/ttyDXL` |
| Windows (smoke test) | `python tools\sim\dxl_smoke_test.py --tcp localhost:5555` |

For ROS 2, set the port of the `dynamixel_hardware_interface` description to
`/tmp/ttyDXL` (or `build-native/ttyDXL`); the baud rate is accepted and
ignored. The pseudoterminal driver polls for the first byte of a packet every
10 ms, so an answer can take up to about 10 ms (a real bus: well under 1 ms);
keep the host's packet timeout above that.

`native_sim` options go after `--`, for example
`run_native.py run -- -stop_at=60` (stop after 60 s) or `-- --no-rt` (as fast
as possible, for tests without a host program).

---

## Tests and CI

The firmware is tested on the PC. ztest suites run the drivers, the queues and the unit
conversions under Twister. Pytest drives the whole `native_sim` firmware with the simulated motor
over Protocol 2.0: the SIL tests use a raw client, `tools/sim/dxl2.py`, and the integration tests
use the official DynamixelSDK in the ros2_control loop.

```bash
docker compose run --rm zephyr bash scripts/run-tests.sh   # tests (task "Zephyr: Run tests (native_sim)")
docker compose run --rm zephyr bash scripts/ci.sh          # layers, builds, all tests, as CI
```

GitHub Actions (`.github/workflows/ci.yml`) runs `scripts/ci.sh` in the environment image of the
repository, published to the GitHub Container Registry, which also serves GitHub Codespaces:
[docs/development.md](docs/development.md#ci-and-the-environment-image). Details:
[docs/testing.md](docs/testing.md). CI cannot see the real hardware:
[docs/hardware-checklist.md](docs/hardware-checklist.md) lists the checks on the board.

---

## Configuration

| What | Where |
|---|---|
| Motor (R, L, rated current, Ke, steps), stage limits, control (microsteps, bandwidth, currents, v_max, accel) | motor node, `app/boards/<board>.overlay` |
| PWM frequency, dead time, ADC channels, shunt, amplifier gain, VBUS divider | power stage node, `app/boards/b_g431b_esc1.overlay` |
| DYNAMIXEL ID, factory baud, start position, gear, NVS partition | servo node, child of the UART |
| Threads, queues, offset samples | `CONFIG_SFS_*`, `app/Kconfig` |
| Stepper-C build options (backend, topologies, sine table, `-O2`) | `CONFIG_SSTEP_*`, `modules/stepper-c/Kconfig` |
| X430 model, devices per bus, protocol features | `CONFIG_X430_*` (Kconfig.x430 of the emulator) |
| Bus buffer, poll period, persistent storage | `CONFIG_SFS_DXL_*`, `drivers/x430/Kconfig` |
| Simulated motor (model parameters, log) | `CONFIG_SFS_SIM_*`, `app/sim.overlay` |
| Board-specific options (FPU, RTT, OPAMP / native_sim tick and logs) | `app/boards/<board>.conf` |

---

## GUI

A desktop GUI on the host (Python, Tkinter) talks to the board over the
DYNAMIXEL bus (ST-LINK Virtual COM Port): motion and live plots with CSV
recording, the whole Control Table, the register checks of the SIL tests as
test scenarios with PASS/FAIL and an HTML report, and the firmware update
through MCUboot serial recovery, verified by the image hash. Task
**GUI: Run** starts `tools/gui/doppelstep_gui.py` from the `.venv` on Windows
and Linux. Manual: [docs/gui/index.md](docs/gui/index.md).

---

## Documentation and PDF manual

`docs/` holds the MkDocs Material sources (`mkdocs.yml`); the PDF manual is
built by `tools/docs/build_pdf.py` (MkDocs + print-site plugin, printed with
Chromium or Microsoft Edge):

```bash
mkdocs serve                       # live preview, task "Docs: Serve"
python tools/docs/build_pdf.py     # site/Doppelstep.pdf, task "Docs: Build PDF"
```

Dependencies: `requirements.txt` (the tasks create the `.venv`).

---

## Bring-up checklist

Follows the Stepper-C integration guide.

1. **Dead time (required).** `dead-time-ns` in
   `app/boards/b_g431b_esc1.overlay` is 0 until the value of the B-G431B-ESC1 MCSDK (Motor Control Workbench)
   project is entered. With 0 the firmware runs, but never enables the gate
   drivers (Torque Enable answers Result Fail, log: "enable refused").
   Check the gate driver input polarity (TIM1 CHx/CHxN active high) against the
   same project.
2. **Outputs off:** RTT log shows the current offsets (≈ 2048 counts) and the
   ADC readings; compare `Present Input Voltage` with a multimeter.
3. **Polarity:** `voltage-drive;` and a low `run-current-milliamp` in the motor node;
   after Torque Enable the measured phase A current must be positive. If not,
   `current-sense-invert;` in the power stage node.
4. **Current loop:** back to current drive, low run current, check the
   current settles at `i_run_ma`.
5. **Motion:** small Goal Position steps (DYNAMIXEL Wizard), then the
   OpenMANIPULATOR-X profile.
6. **Protection:** lowered `trip-current-milliamp` / `vbus-max-mv`, check
   the trips and the Hardware Error Status.

---

## Open items

- `dead-time-ns`: value from the ESC1 MCSDK project (see above).
- Stall detection: a lost synchronism (load above the holding torque at the hold
  current) is not reported to the host; `tests/sil/test_protection.py` documents it
  as an expected failure.
- The driver refactor is not yet checked on the board:
  [docs/hardware-checklist.md](docs/hardware-checklist.md).
- Rotor vibration at rest in the model: one ADC step in the current loop excites the mechanical
  resonance (up to about 3 rpm, 0.2 microstep). Not measured on a real motor (same checklist).
- Fast-loop execution time on the target (Stepper-C: measure on the MCU); the
  RTT log of the simulated motor shows the longest fast-loop callback.
- Present Temperature: NTC on PB14 is not converted yet (no NTC data in UM2516);
  the register keeps its table value and there is no overheating trip.
- Single-wire TTL bus instead of the VCP, and a shared bus with real DYNAMIXEL
  actuators.
- `.agents/skills`: a common `sfs-development-workspace` skill before release.

---

## Naming

**Doppelstep** is the public name; in identifiers it is written `doppelstep`
(west path, Docker image and volume, CMake project, Zephyr module). The internal name was
`sfs-stepper-driver`. The `SFS_` prefix of the application Kconfig symbols and
the `sfs,` devicetree vendor prefix stand for SystemFromScratch and stay
unchanged. The story behind the name: [docs/NAMING.md](docs/NAMING.md).

---

## Licenses

Application, drivers, board, module glue and configuration: Apache License 2.0
([LICENSE](LICENSE)). Zephyr RTOS, the Zephyr SDK, Stepper-C, the DYNAMIXEL X430
emulator, DXL2-C, STM32Cube HAL/LL and SEGGER RTT come under their own licenses.
The SEGGER J-Link Software is not part of the repository or the image unless
you add it yourself under SEGGER's license.

DYNAMIXEL and ROBOTIS are trademarks of ROBOTIS Co., Ltd.; STM32 is a registered
trademark of STMicroelectronics International N.V. This project is not
affiliated with or endorsed by them.
