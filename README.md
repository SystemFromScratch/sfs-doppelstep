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

The build result is `build/zephyr/zephyr.elf`; flashing and debugging work with
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
- [Building](#building)
- [Flashing and debugging](#flashing-and-debugging)
- [Logs (RTT)](#logs-rtt)
- [DYNAMIXEL emulation](#dynamixel-emulation)
- [Configuration](#configuration)
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
 ┌──────────────────────────────┐   UART ISR → ring buffer → thread "dxl"
 │ dxl_bus.c                    │   x430_bus_receive / x430_bus_poll
 │  X430 emulator (XM430-W350)  │   hooks: Torque Enable, Goal Position, ...
 │  joint.c (unit conversions)  │   live registers from the motor snapshot
 └──────────────┬───────────────┘
                │ motor_*() commands (synchronous)      ▲ snapshot
 ┌──────────────▼───────────────┐                       │
 │ motor.c   thread "motor"     │ sstep_slow_loop() at 1 kHz, every command
 │  Stepper-C (float backend)   │
 └──────────────┬───────────────┘
                │ sstep_fast_loop() in the zero-latency ADC interrupt, 20 kHz
 ┌──────────────▼───────────────┐
 │ power_stage.c                │ TIM1 center-aligned complementary PWM (U,V,W)
 │  OPAMP1..3 (Zephyr driver)   │ ADC1 (iU, VBUS, iW) + ADC2 (iV), injected,
 │  TIM1 / ADC1 / ADC2 (LL)     │ triggered by TIM1 TRGO2 at the counter top
 └──────────────────────────────┘
                │
        L6387 gate drivers → 3 half-bridges → stepper motor
        phase A between U and W, phase B between V and W (shared-leg topology)
```

| Context | Rate | Work |
|---|---|---|
| ADC1_2 interrupt (zero latency, priority 0) | PWM rate, 20 kHz | read currents and VBUS, shared-leg (W) trip, `sstep_fast_loop()` |
| thread `motor` (cooperative, −2) | 1 kHz, absolute schedule | `sstep_slow_loop()`, queued commands, status snapshot |
| thread `dxl` (preemptive, 2) | on RX + every 10 ms | X430 parser and answers, Bus Watchdog, live registers |
| `main` | — | startup, STATUS LED |

---

## Repository structure

```text
doppelstep/                             west manifest repository (this repo)
├── west.yml                            Zephyr v4.5.0, Stepper-C v1.0.0, X430 emulator (main), DXL2-C v1.0.0
├── app/                                Zephyr application
│   ├── CMakeLists.txt                  BOARD_ROOT, DTS_ROOT, EXTRA_ZEPHYR_MODULES
│   ├── Kconfig                         motor, power stage, control, DYNAMIXEL options
│   ├── prj.conf
│   ├── boards/b_g431b_esc1.overlay     power stage node, DYNAMIXEL UART (1 Mbps)
│   └── src/
│       ├── main.c                      startup, STATUS LED
│       ├── power_stage.[ch]            TIM1 + ADC1/ADC2 (LL) + OPAMPs, fast-loop ISR
│       ├── motor.[ch]                  Stepper-C instance, slow-loop thread, commands
│       ├── dxl_bus.[ch]                UART, X430 bus/device, hooks, events
│       ├── joint.[ch]                  microsteps <-> DYNAMIXEL units, profiles
│       └── nvm.[ch]                    EEPROM area / backup in flash (NVS)
├── boards/st/b_g431b_esc1/             out-of-tree board (not in Zephyr 4.5)
│   ├── b_g431b_esc1.dts                pinout from UM2516 Table 4, 170 MHz from 8 MHz HSE
│   ├── board.cmake                     runners: openocd (default), jlink, stm32cubeprogrammer, pyocd
│   └── support/openocd.cfg             ST-LINK/V2-1, STM32G4
├── dts/bindings/sfs/                   sfs,stm32-shared-leg-power-stage
├── modules/                            Zephyr module glue for the libraries (workspace side)
│   ├── stepper-c/zephyr/               module.yml, Kconfig (CONFIG_SSTEP_*), CMakeLists.txt
│   └── dynamixel-x430/zephyr/          module.yml, Kconfig (sources Kconfig.x430), CMakeLists.txt
├── scripts/
│   ├── west-init.sh                    west init -l + west update (in the container)
│   └── host/                           GDB servers on the Windows host (OpenOCD, J-Link)
├── docker/jlink/                       optional J-Link Software archive for the image
├── .devcontainer/
│   ├── devcontainer.json               default: GDB server on the host (mode B)
│   └── usb-passthrough/devcontainer.json  probe in the container via usbipd (mode A)
├── .vscode/                            tasks (west menu), Cortex-Debug launch configurations
├── docs/NAMING.md                   where the name Doppelstep comes from
├── .agents/skills/doppelstep/      engineering rules for agents
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
    └── lib/stepper-c  lib/dynamixel-x430  lib/dxl2-c
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
| Stepper-C | **v1.0.0** | `west.yml` |
| DYNAMIXEL-X430 emulator | `main` (until its first release) | `west.yml` |
| DXL2-C | v1.0.0 (= `X430_DXL2_GIT_TAG` of the emulator) | `west.yml` |
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
on `|iW| > i_trip` (`CONFIG_SFS_STAGE_PHASE_W_TRIP`).

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

## Building

```bash
west build -b b_g431b_esc1 -s app -d build               # incremental
west build -p always -b b_g431b_esc1 -s app -d build     # pristine
west build -d build -t menuconfig                        # Kconfig
```

`Ctrl+Shift+B` opens the **Zephyr: Workspace Menu** (initialize/update, build,
pristine build, menuconfig, flash, RTT).

Artifacts in `build/zephyr/`: **`zephyr.elf`** (debugging), `zephyr.hex`,
`zephyr.bin`. Current size: about 70 KiB flash of the 124 KiB code partition,
18 KiB RAM.

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

Every *Debug* configuration builds first, loads `zephyr.elf`, resets and stops
at `main`.

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
Sync/Bulk Read, Bus Watchdog …); the firmware maps the registers onto Stepper-C:

| Register | Mapping |
|---|---|
| Torque Enable (64) | `sstep_enable()` / `sstep_disable()`; refused (Result Fail) while the dead time is not set or with a fault |
| Operating Mode (11) | 3 position, 4 extended position, 1 velocity; 0, 5, 16 are refused (Data Range Error) |
| Goal Position (116) | `sstep_move_to()`; limits from Profile Velocity/Acceleration (velocity- or time-based, Drive Mode bit 2), Velocity Limit and `CONFIG_SFS_CTRL_V_MAX` / `ACCEL_MAX` |
| Goal Velocity (104) | `sstep_run()` (velocity mode) |
| Drive Mode (10) bit 0, Homing Offset (20) | direction and offset of the position mapping |
| Current Limit (38) | run current = min(limit × 2.69 mA, `CONFIG_SFS_CTRL_I_RUN_MA`) |
| Present Position (132), Position Trajectory (140) | commanded position (no encoder) |
| Present Velocity (128), Velocity Trajectory (136) | commanded velocity |
| Present Current (126) | phase current amplitude √(iA² + iB²) |
| Present Input Voltage (144) | measured VBUS (0.1 V) |
| Moving (122), Moving Status (123) | from the Stepper-C state |
| Hardware Error Status (70) | over/undervoltage → Input Voltage; overcurrent, W trip, HAL → Electrical Shock |
| Bus Watchdog | `sstep_stop()` |
| REBOOT / FACTORY_RESET | outputs off, faults cleared |
| CLEAR (multi-turn) | position redefined within one turn |
| Baud Rate (8) | USART2 reconfigured after the status packet |

Unit conversions (`app/src/joint.c`): 4096 pulses per joint revolution,
0.229 rev/min, 214.577 rev/min², 2.69 mA; 3200 microsteps per motor revolution
(200 × 16), gear ratio `CONFIG_SFS_DXL_GEAR_MOTOR_REVS / CONFIG_SFS_DXL_GEAR_JOINT_REVS`
(1/1).

**Startup position:** the motor has no encoder. At power-on the rotor position
is reported as Present Position **2048** (`CONFIG_SFS_DXL_START_POSITION`,
0 rad for `dynamixel_hardware_interface`): power the joint up in that position.
With torque off the rotor can be moved by hand and the reported position is
then no longer the real one.

**Factory values of this firmware:** ID `CONFIG_SFS_DXL_ID` (1) and Baud Rate
code `CONFIG_SFS_DXL_FACTORY_BAUD_CODE` (3 = 1 Mbps, the OpenMANIPULATOR-X bus
speed). They apply while no EEPROM block is stored and after FACTORY_RESET; a
real XM430 leaves the factory at 57600 bps. ID and Baud Rate written over the
bus are stored in flash (NVS, `storage_partition`); flash writes are refused
while the power stage is enabled (single-bank flash: an erase stalls the CPU).

**OpenMANIPULATOR-X:** its `ros2_control` description expects five
XM430-W350 on one bus (IDs 11–15, 1 Mbps, Operating Mode 3, Drive Mode 4 =
time-based profile). This firmware answers for **one** ID, so over the VCP the
host sees one joint; set `CONFIG_SFS_DXL_ID` to the joint's ID (for example
11). Fast Sync/Bulk Read answers come only from the IDs of one emulator bus.

---

## Configuration

| What | Where |
|---|---|
| Motor (R, L, rated current, Ke, steps), stage limits, control (microsteps, bandwidth, currents, v_max, accel) | `app/Kconfig` (defaults: Stepper-C documentation, 17HS19-2004S1 on B-G431B-ESC1) |
| PWM frequency, dead time, ADC channels, shunt, amplifier gain, VBUS divider | `app/boards/b_g431b_esc1.overlay` (power-stage node) |
| Stepper-C build options (backend FLOAT, topologies, sine table) | `CONFIG_SSTEP_*` (`modules/stepper-c/zephyr/Kconfig`) |
| X430 model, devices per bus, protocol features | `CONFIG_X430_*` (Kconfig.x430 of the emulator) |
| DYNAMIXEL ID, factory baud, start position, gear | `CONFIG_SFS_DXL_*` |

---

## Bring-up checklist

Follows the Stepper-C integration guide.

1. **Dead time (required).** `dead-time-ns` in `app/boards/b_g431b_esc1.overlay`
   is 0 until the value of the B-G431B-ESC1 MCSDK (Motor Control Workbench)
   project is entered. With 0 the firmware runs, but never enables the gate
   drivers (Torque Enable answers Result Fail, log: "dead-time-ns … not set").
   Check the gate driver input polarity (TIM1 CHx/CHxN active high) against the
   same project.
2. **Outputs off:** RTT log shows the current offsets (≈ 2048 counts) and the
   ADC readings; compare `Present Input Voltage` with a multimeter.
3. **Polarity:** `CONFIG_SFS_CTRL_VOLTAGE_DRIVE=y`, low `CONFIG_SFS_CTRL_I_RUN_MA`;
   after Torque Enable the measured phase A current must be positive. If not,
   `CONFIG_SFS_STAGE_CURRENT_SENSE_INVERT=y`.
4. **Current loop:** back to current drive, low run current, check the
   current settles at `i_run_ma`.
5. **Motion:** small Goal Position steps (DYNAMIXEL Wizard), then the
   OpenMANIPULATOR-X profile.
6. **Protection:** lowered `CONFIG_SFS_STAGE_I_TRIP_MA` / `VBUS_MAX_MV`, check
   the trips and the Hardware Error Status.

---

## Open items

- `dead-time-ns`: value from the ESC1 MCSDK project (see above).
- Fast-loop execution time on the target (Stepper-C: measure on the MCU).
- Present Temperature: NTC on PB14 is not converted yet (no NTC data in UM2516);
  the register keeps its table value and there is no overheating trip.
- Single-wire TTL bus instead of the VCP, and a shared bus with real DYNAMIXEL
  actuators.
- `.agents/skills`: a common `sfs-development-workspace` skill before release.

---

## Naming

**Doppelstep** is the public name; in identifiers it is written `doppelstep`
(west path, Docker image and volume, CMake project). The internal name was
`sfs-stepper-driver`. The `SFS_` prefix of the application Kconfig symbols and
the `sfs,` devicetree vendor prefix stand for SystemFromScratch and stay
unchanged. The story behind the name: [docs/NAMING.md](docs/NAMING.md).

---

## Licenses

Application, board, module glue and configuration: Apache License 2.0
([LICENSE](LICENSE)). Zephyr RTOS, the Zephyr SDK, Stepper-C, the DYNAMIXEL X430
emulator, DXL2-C, STM32Cube HAL/LL and SEGGER RTT come under their own licenses.
The SEGGER J-Link Software is not part of the repository or the image unless
you add it yourself under SEGGER's license.

DYNAMIXEL and ROBOTIS are trademarks of ROBOTIS Co., Ltd.; STM32 is a registered
trademark of STMicroelectronics International N.V. This project is not
affiliated with or endorsed by them.
