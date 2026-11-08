<p align="center">
  <img src="docs/assets/images/hero-3d.svg" width="640"
       alt="DYNAMIXEL Protocol 2.0 packets from a ROS 2 host reach a controller board running Doppelstep on Zephyr RTOS, which drives a stepper motor with a servo horn through two phase currents 90 degrees apart">
</p>

<h1 align="center">Doppelstep</h1>

<p align="center">
  <b>Your stepper, seen as a smart servo: Zephyr RTOS firmware that turns a stepper motor and an STM32 board into a DYNAMIXEL XM430-W350.</b><br>
  Zephyr RTOS 4.5 · DYNAMIXEL Protocol 2.0 · 20 kHz current control · MCUboot · Tested on a PC
</p>

<p align="center">
  <a href="https://github.com/SystemFromScratch/sfs-doppelstep/actions/workflows/ci.yml"><img src="https://github.com/SystemFromScratch/sfs-doppelstep/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="https://github.com/SystemFromScratch/sfs-doppelstep/actions/workflows/docs.yml"><img src="https://github.com/SystemFromScratch/sfs-doppelstep/actions/workflows/docs.yml/badge.svg?branch=main" alt="Documentation"></a>
  <a href="https://github.com/SystemFromScratch/sfs-doppelstep/releases/latest"><img src="https://img.shields.io/github/v/release/SystemFromScratch/sfs-doppelstep?include_prereleases&sort=semver" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License: Apache-2.0"></a>
</p>

<p align="center">
  <a href="https://systemfromscratch.github.io/sfs-doppelstep/">Documentation</a> ·
  <a href="https://systemfromscratch.github.io/sfs-doppelstep/Doppelstep.pdf">PDF manual</a> ·
  <a href="https://systemfromscratch.github.io/sfs-doppelstep/getting-started/">Getting started</a> ·
  <a href="https://systemfromscratch.github.io/sfs-doppelstep/hardware/">Hardware and wiring</a>
</p>

---

## Overview

Doppelstep is open-source Zephyr RTOS firmware for the STMicroelectronics **B-G431B-ESC1**
motor-control board. It drives a two-phase stepper motor with closed-loop microstepping
current control and answers on the bus as a **DYNAMIXEL XM430-W350** (model 1020). ROS 2
`dynamixel_hardware_interface`, DynamixelSDK and DYNAMIXEL Wizard talk to it unchanged, for
example as a joint of an OpenMANIPULATOR-X robot arm. The same firmware runs on a PC with a
physical model of the motor, so protocol, motion and protection are tested on every change
before they reach the hardware.

| | |
| --- | --- |
| **Servo emulation** | The complete Control Table of an XM430-W350, checked as the real servo checks it; position, extended position and velocity mode; Sync/Bulk and Fast Sync/Bulk Read, Bus Watchdog, Reboot, Factory Reset; EEPROM area in flash |
| **Motor control** | [Stepper-C](https://github.com/AntonVynohradov/Stepper-C): current loop at 20 kHz in a zero-latency interrupt, motion profiles at 1 kHz, 16 microsteps per full step |
| **Protection** | Overcurrent, bus voltage and shared-leg faults switch the outputs off inside the 20 kHz interrupt, independent of any thread |
| **Zephyr drivers** | A `power_stage` driver class with one backend per MCU family, the motor behind Zephyr's `stepper` / `stepper_ctrl` APIs, the servo as a child node of its UART, an out-of-tree board; layer rules checked in CI |
| **Firmware update** | MCUboot with an RSA-2048 signature check; serial recovery over the DYNAMIXEL bus itself |
| **Simulation** | The plant model of Stepper-Lab with power stage values from ngspice, on the PC (`native_sim`) or on the board |
| **Bench GUI** | Motion with live plots and CSV recording, the whole Control Table, the SIL test scenarios on the board with an HTML report, firmware update |

Measured with the CI build of this version:

| | |
| --- | --- |
| Board | B-G431B-ESC1: STM32G431CB, Cortex-M4F, 170 MHz, 128 KiB flash, 32 KiB RAM |
| Application | 77 356 B flash of the 87 712 B slot, 19 032 B RAM |
| Bootloader | MCUboot, 37 148 B flash |
| Tests per CI run | 91: 55 ztest cases, 28 SIL and integration tests, 8 fixed-point motion tests |

## Why it is worth a look

| | |
| --- | --- |
| **Drop-in for the host** | The host sees an XM430-W350, not a stepper. An integration test runs the ros2_control loop with the official DynamixelSDK at 100 Hz against the firmware. [DYNAMIXEL emulation](https://systemfromscratch.github.io/sfs-doppelstep/dynamixel/) |
| **Real-time motor control** | Closed-loop microstepping current control in a 20 kHz zero-latency interrupt; a fault switches the outputs off inside that interrupt. [Architecture](https://systemfromscratch.github.io/sfs-doppelstep/architecture/) |
| **Tested before hardware** | The same firmware runs on a PC with the motor model of Stepper-Lab and power stage values from ngspice: 91 automated tests on every CI run. [Simulated motor](https://systemfromscratch.github.io/sfs-doppelstep/simulation/) |
| **Real Zephyr engineering** | Own driver class for the power stage, the motor behind Zephyr's `stepper` / `stepper_ctrl` APIs, an out-of-tree board, devicetree bindings, layer rules enforced in CI. [Porting to a new MCU family](https://systemfromscratch.github.io/sfs-doppelstep/porting/) |
| **Field update over the servo bus** | MCUboot serial recovery with an RSA-2048 signature; the bench GUI verifies the uploaded image by its hash. [Firmware update](https://systemfromscratch.github.io/sfs-doppelstep/firmware-update/) |
| **Reproducible anywhere** | Pinned versions, a Docker image published for CI and GitHub Codespaces, a Dev Container, and the same VS Code tasks on Windows and Linux. [Development environment](https://systemfromscratch.github.io/sfs-doppelstep/development/) |

## Project status

Doppelstep **0.1.0** is the first release: verified on the PC against the simulated motor, not
yet on a real motor (see the first open item). Changes per release are in
[CHANGELOG.md](CHANGELOG.md); the open items are listed in
[Status and known limits](https://systemfromscratch.github.io/sfs-doppelstep/overview/#status-and-known-limits).

- [x] XM430-W350 emulation over Protocol 2.0, mapped onto the motor
- [x] Closed-loop microstepping current control with protection in the interrupt
- [x] SIL and integration tests on `native_sim`, CI in a published environment image
- [x] MCUboot with serial recovery over the bus; bench GUI
- [x] Documentation website and PDF manual
- [ ] Dead time of the power stage: until it is set, the firmware never enables the outputs
- [ ] The latest driver refactor checked on the board ([hardware checklist](https://systemfromscratch.github.io/sfs-doppelstep/hardware-checklist/))
- [ ] Lost steps reported (the motor has no encoder)
- [ ] Temperature sensor and overheating trip
- [ ] Single-wire TTL bus shared with real DYNAMIXEL servos

## Quick start

No hardware needed. In the browser:

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/SystemFromScratch/sfs-doppelstep?quickstart=1&devcontainer_path=.devcontainer%2Fcodespaces%2Fdevcontainer.json)

```bash
bash scripts/run-tests.sh          # firmware on native_sim + 28 SIL and integration tests
```

Or on your PC, with Docker and Git:

```bash
git clone https://github.com/SystemFromScratch/sfs-doppelstep.git doppelstep
cd doppelstep
docker compose build                                         # toolchain image
docker compose run --rm zephyr bash scripts/west-init.sh     # Zephyr and the libraries
docker compose run --rm zephyr bash scripts/run-tests.sh     # firmware + SIL and integration tests
```

Then run the simulated servo and move it from the PC, the way a robot controller would:

```bash
docker compose run --rm zephyr west build -b native_sim/native/64 -s app -d build-native
docker compose run --rm -p 5555:5555 zephyr python3 tools/sim/run_native.py run --tcp 5555
python tools/sim/dxl_smoke_test.py --tcp localhost:5555      # second terminal
```

## Prebuilt firmware

Every [release](https://github.com/SystemFromScratch/sfs-doppelstep/releases) carries the
board images, built and tested by the release pipeline: `…-merged.hex` (MCUboot and the
application) for the first flashing through the ST-LINK, the signed `…-app.signed.bin` for an
update over the bus with the GUI, the same pair with the simulated motor (`…-sim-motor.*`), the
PDF manual and `SHA256SUMS`. The images are signed with the public development key of
MCUboot: for a product, build from source with your own key
([Firmware update](https://systemfromscratch.github.io/sfs-doppelstep/firmware-update/)).

## Building

| Target | Command |
| --- | --- |
| Board, real motor | `west build -p always --sysbuild -b b_g431b_esc1 -s app -d build` |
| Board, simulated motor | the same, plus `-- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay` |
| PC (`native_sim`) | `west build -b native_sim/native/64 -s app -d build-native` |
| Flash MCUboot and the signed application | `west flash -d build -r openocd` |

Run `west` in the container (`docker compose run --rm zephyr …`), in the Dev Container or in
a codespace; the VS Code tasks do the same on Windows and Linux. Before a real motor turns,
wire it as shown in
[Hardware and bring-up](https://systemfromscratch.github.io/sfs-doppelstep/hardware/#motor-wiring)
and follow its bring-up checklist.

## Project structure

```text
app/                    Zephyr application: threads, queues, register mapping, board overlays
drivers/                power_stage/ (STM32, simulation), motor/ (Stepper-C), x430/ (servo)
include/, dts/          public driver APIs, devicetree bindings
boards/                 out-of-tree board B-G431B-ESC1
modules/                build glue of Stepper-C, the X430 emulator and Stepper-Lab
tests/                  ztest suites, pytest SIL and integration tests
tools/                  bench GUI, native_sim launcher, documentation tools
scripts/                CI, tests, layer check, release packaging, host GDB servers
docs/                   documentation website and PDF manual sources
west.yml                pinned Zephyr and libraries
CHANGELOG.md            changes of every release
```

## Documentation

The website at **[systemfromscratch.github.io/sfs-doppelstep](https://systemfromscratch.github.io/sfs-doppelstep/)**
covers the idea and the technology stack, the hardware with the motor wiring, the DYNAMIXEL
emulation, configuration, architecture, the simulated motor, firmware update, porting, the
development environment, tests and the bench GUI. The same content is available as a
printable [PDF manual](https://systemfromscratch.github.io/sfs-doppelstep/Doppelstep.pdf).

```bash
python -m pip install -r requirements.txt
mkdocs serve                                  # live preview
python tools/docs/build_manual.py             # PDF manual
python tools/docs/diagrams.py                 # regenerate the diagrams
```

## Author

**Anton Vynohradov** — Embedded Systems Engineer, founder of SystemFromScratch®: firmware and
embedded Linux for connected products, secure, MISRA-compliant and ready for the EU Cyber
Resilience Act.
[systemfromscratch.github.io](https://systemfromscratch.github.io/) ·
[GitHub](https://github.com/AntonVynohradov) ·
[LinkedIn](https://www.linkedin.com/in/vynohradov) ·
[ORCID](https://orcid.org/0009-0005-6965-0911) ·
avynohradov@systemfromscratch.com

Need firmware like this for your product, Zephyr drivers, a bootloader or CRA readiness? Get
in touch.

## Contributing

Issues and pull requests are welcome, measurements from a real B-G431B-ESC1 especially. A
change must keep the builds free of warnings, pass the layer check and all tests
(`scripts/ci.sh`) and keep the documentation in sync. See [CONTRIBUTING.md](CONTRIBUTING.md);
security issues: [SECURITY.md](SECURITY.md).

## License

Copyright © 2026 Anton Vynohradov. Licensed under the [Apache License 2.0](LICENSE). Zephyr
RTOS, MCUboot, the libraries and the tools Doppelstep uses come under their own licenses
([License](https://systemfromscratch.github.io/sfs-doppelstep/license/)).

SystemFromScratch® is a registered trademark. The Apache License 2.0 grants no rights to use
the SystemFromScratch® name or logo; they may not be used to endorse or promote products
derived from Doppelstep without prior written permission. DYNAMIXEL and ROBOTIS are
trademarks of ROBOTIS Co., Ltd.; STM32 is a registered trademark of STMicroelectronics
International N.V. Doppelstep is not affiliated with or endorsed by them.

## References

- [Documentation](https://systemfromscratch.github.io/sfs-doppelstep/) and
  [PDF manual](https://systemfromscratch.github.io/sfs-doppelstep/Doppelstep.pdf)
- [Stepper-C](https://antonvynohradov.github.io/Stepper-C/),
  [DXL2-C](https://antonvynohradov.github.io/DXL2-C/),
  [Stepper-Lab](https://antonvynohradov.github.io/Stepper-Lab/) and the
  [DYNAMIXEL X430 Control Table Emulator](https://github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator)
  (no documentation website yet)
- [Zephyr RTOS](https://docs.zephyrproject.org/) and [MCUboot](https://docs.mcuboot.com/)
- [DYNAMIXEL Protocol 2.0](https://emanual.robotis.com/docs/en/dxl/protocol2/) and the
  [XM430-W350 Control Table](https://emanual.robotis.com/docs/en/dxl/x/xm430-w350/)
