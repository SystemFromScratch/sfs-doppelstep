# Technology stack

Doppelstep is built from established open-source components and from the libraries of the
SystemFromScratch® portfolio. This page lists every one of them: what it does in the project,
which version is used, and why it was chosen. All versions are pinned. The source of truth is
`west.yml` for the firmware, the `Dockerfile` for the toolchain, and the requirement files for
the Python tools.

## At a glance

| Layer | Technology |
|---|---|
| Real-time operating system | **Zephyr RTOS v4.5.0**: drivers, devicetree, Kconfig, threads, sysbuild |
| Bootloader | **MCUboot** with an RSA-2048 signature check and serial recovery |
| Motor control | **Stepper-C v1.2.0**: closed-loop microstepping current control |
| Servo protocol | **DYNAMIXEL X430 Control Table Emulator** on **DXL2-C v1.0.0** (Protocol 2.0) |
| Simulation | **Stepper-Lab** plant model, power stage values from **ngspice** (sfs-stepper-power-spice), Zephyr **native_sim** |
| Hardware access | STM32Cube **LL drivers** (hal_stm32), **CMSIS 6**, **SEGGER RTT** |
| Tests | Zephyr **ztest** and **Twister**, **pytest**, the official **DynamixelSDK** |
| Build and environment | **west**, **Zephyr SDK 1.0.1**, **CMake**, **Docker**, Dev Container, **GitHub Codespaces** |
| CI | **GitHub Actions** with an environment image in the **GitHub Container Registry**, ccache |
| Host tools | **Python**: Tkinter GUI, **matplotlib**, **pyserial**, **smpmgr** / smpclient |
| Debugging | **OpenOCD** (ST-LINK), **SEGGER J-Link**, VS Code **Cortex-Debug** |
| Documentation | **MkDocs Material**, a PDF manual printed with Chromium |

## Firmware platform

### Zephyr RTOS

| Item | Detail |
|---|---|
| Version | v4.5.0 (`west.yml`) |
| Role | Kernel, threads and message queues, UART and flash drivers, NVS storage, logging, the devicetree and Kconfig configuration, the build system (sysbuild) |
| Why | A vendor-neutral RTOS with a long-term support model and a driver model that separates hardware from application. The same application builds for the STM32 board and for the PC (`native_sim`), which is what makes the simulated tests possible |

Doppelstep is a proper **Zephyr module**, laid out like the official `example-application`:
its own drivers, devicetree bindings with the `sfs,` vendor prefix, and an out-of-tree board
definition for the B-G431B-ESC1, which Zephyr 4.5 does not include. The motor is exposed
through Zephyr's standard `stepper` and `stepper_ctrl` driver classes, so other Zephyr code can
drive it without knowing Stepper-C ([Architecture](architecture.md#drivers)). Only the modules
the firmware needs are imported (`name-allowlist` in `west.yml`), which keeps the workspace
small.

### MCUboot

| Item | Detail |
|---|---|
| Version | The revision pinned by Zephyr v4.5.0, with mbedtls, tf-psa-crypto and zcbor |
| Role | Secure bootloader: checks the RSA-2048 signature of the application at every start; serial recovery accepts a new image over the DYNAMIXEL bus (SMP protocol) |
| Why | The standard bootloader of the Zephyr ecosystem, built together with the application by sysbuild. Field updates need only the bus cable, and foreign images do not start |

Details: [Firmware update](firmware-update.md).

### STM32Cube LL, CMSIS 6, SEGGER RTT

| Component | Role | Why |
|---|---|---|
| **hal_stm32** (STM32Cube LL drivers) | Timer TIM1, ADC1/ADC2 injected conversions, OPAMP1..3 in the power stage backend | Zephyr's PWM and ADC APIs have no dead time, complementary outputs or timer-triggered injected conversions, which a 20 kHz current loop needs. LL code stays in one file |
| **CMSIS 6** | Core access for the Cortex-M4F: zero-latency interrupts, the cycle counter | Arm's standard core interface |
| **SEGGER RTT** | Console and log through the debug probe | USART2 carries the servo bus, so the log needs another channel; RTT costs almost no CPU time |

## Motor control

### Stepper-C

| Item | Detail |
|---|---|
| Version | v1.2.0 (`west.yml`) |
| Source | [github.com/AntonVynohradov/Stepper-C](https://github.com/AntonVynohradov/Stepper-C), SystemFromScratch® |
| Role | Closed-loop microstepping current control of a two-phase stepper motor: the current controller (fast loop, 20 kHz), the motion profiles (slow loop, 1 kHz), current and voltage protection |
| Why | A hardware-agnostic library in portable C, with no heap and no RTOS, that reaches the hardware only through callbacks. Doppelstep wraps it in a Zephyr driver instead of re-implementing control in the application |

Build options used here (`modules/stepper-c/Kconfig`):

| Option | Doppelstep uses |
|---|---|
| Numeric backend | Single-precision float on the STM32G431 (hardware FPU). The fixed-point backend (Q15/Q31, integer only) is tested in CI for cores without an FPU |
| Power stage topology | Three half-bridges with a shared leg: the B-G431B-ESC1 has three legs, not four |
| Microstepping | 16 microsteps per full step, 3200 per revolution |

## Servo protocol

### DYNAMIXEL X430 Control Table Emulator

| Item | Detail |
|---|---|
| Version | `main` until its first release (`west.yml`) |
| Source | [github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator](https://github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator), SystemFromScratch® |
| Role | The complete Control Table of an XM430-W350 (model 1020): access rules, ranges and limits, the EEPROM lock, Indirect Addresses, Sync/Bulk and Fast Sync/Bulk Read, Bus Watchdog, Reboot and Factory Reset |
| Why | The servo's behaviour is far more than reading and writing registers. A dedicated, separately tested emulator keeps that behaviour exact, and keeps DYNAMIXEL details out of the motor code |

### DXL2-C

| Item | Detail |
|---|---|
| Version | v1.0.0 (`west.yml`), the tag the emulator expects |
| Source | [github.com/AntonVynohradov/DXL2-C](https://github.com/AntonVynohradov/DXL2-C), SystemFromScratch® |
| Role | DYNAMIXEL Protocol 2.0 packet codec and link layer: framing, byte stuffing, CRC |
| Why | A released protocol library of the portfolio. The emulator normally downloads it with CMake FetchContent; the module glue points `FETCHCONTENT_SOURCE_DIR_DXL2` at the west project instead, so a build needs no network access, and warns when the `dxl2-c` revision in `west.yml` differs from the tag the emulator expects |

## Simulation

| Component | Version | Role | Why |
|---|---|---|---|
| **Stepper-Lab** | `main` until its first release | The motor and power stage model in C (`sil/c/sstep_plant.c`) and its parameters (`params/*.json`) | It is the same C code as the Xcos block of the Scilab prototype, so the firmware is tested against the model the controller was designed with |
| **sfs-stepper-power-spice** | `main` until its first release | ngspice model of one leg of the B-G431B-ESC1; writes the stage parameters of Stepper-Lab | The leg voltage error (dead time, body diodes, resistance) comes from a circuit simulation, not from a guess |
| **Zephyr native_sim** | part of Zephyr v4.5.0 | Builds the whole firmware as a Linux program | The complete chain, protocol to motor model, runs on any PC and in CI |

How the three repositories work together: [Simulated motor](simulation.md).

## Tests and continuous integration

| Component | Version | Role |
|---|---|---|
| **ztest** and **Twister** | Zephyr v4.5.0 | 55 test cases in 4 suites: unit conversions, queues, the power stage API, the motor driver on a fake power stage |
| **pytest** (+ pytest-timeout) | 9.0.2 (2.4.0) | 28 SIL and integration tests against the `native_sim` firmware over Protocol 2.0, plus 8 motion tests on the fixed-point backend |
| **DynamixelSDK** | 4.1.0 | The official ROBOTIS library under ROS 2 `dynamixel_hardware_interface`: the integration tests run the ros2_control loop with it |
| **Layer check** | `scripts/check-layers.sh` | Enforces the architecture rules: no register access outside the backends, no floating point above them, no DYNAMIXEL code in the motor driver |
| **GitHub Actions** | `.github/workflows/ci.yml` | Runs `scripts/ci.sh` on every push to `main` and every pull request: layer check, three pristine builds, all tests |
| **GitHub Container Registry** | `ghcr.io/systemfromscratch/sfs-doppelstep-env` | The environment image with the toolchain and the west workspace, tagged by the hash of the files it is made from |
| **ccache** | in the image | Keeps CI and local rebuilds fast |

Details: [Tests and CI](testing.md).

## Build and development environment

| Component | Version | Role |
|---|---|---|
| **Docker** image | base `python:3.12.15-slim-bookworm`, digest pinned | The whole toolchain in one image, identical on Windows, Linux, CI and Codespaces |
| **Zephyr SDK** | 1.0.1 (minimal, `arm-zephyr-eabi`, host tools with OpenOCD) | GCC toolchain for Arm and the host tools |
| **west** | 1.5.0 | Workspace manager and build front end of Zephyr |
| **CMake** | 3.30.5 | Build system under west |
| **VS Code** tasks and **Dev Container** | `.vscode/`, `.devcontainer/` | The same tasks on Windows and Linux; a container with the debug probe passed through USB or a debug server on the host |
| **GitHub Codespaces** | `.devcontainer/codespaces/` | Builds and the simulated tests in the browser, nothing to install |
| **OpenOCD**, **SEGGER J-Link**, **Cortex-Debug** | SDK / host installation | Flashing, debugging with Zephyr thread awareness, RTT log |

Details: [Development environment](development.md).

## Host tools

| Component | Version | Role |
|---|---|---|
| **Python** with **Tkinter** | 3.11 or newer | The bench GUI: motion, live plots, Control Table, test scenarios, firmware update |
| **matplotlib** | 3.11.2 | Live plots of position, velocity, current and voltage |
| **pyserial** | 3.5 | The serial link to the bus |
| **smpmgr** / smpclient | 0.19.1 | Firmware upload through MCUboot serial recovery, from the command line or the GUI |

Details: [GUI](gui/index.md).

## Documentation

| Component | Role |
|---|---|
| **MkDocs** with **Material for MkDocs** | This website, in the SystemFromScratch® style, built in strict mode (a broken link fails the build) |
| **mkdocs-print-site-plugin**, **Playwright** / Chromium, **pypdf** | The printable PDF manual from the same sources, with a cover, a table of contents with page numbers and bookmarks |

## Engineering practices

The technologies matter less than how they are used. These rules hold across the project:

| Practice | In Doppelstep |
|---|---|
| Everything pinned | Zephyr, libraries, SDK, Python packages and the base image have fixed versions or digests; the CI image is tagged by the hash of its inputs |
| Layers enforced by a tool | The architecture rules are checked in CI, not only written down |
| Builds without warnings | The CI builds are kept free of warnings; the one known exception, a warning of the X430 emulator under host gcc, is reported upstream |
| Hardware isolated | One backend file per MCU family touches registers; the rest is portable C on Zephyr APIs |
| Tests before hardware | Protocol, motion and protection are tested on the PC against a physical model on every change |
| Measured numbers only | Sizes, rates and test counts in this documentation come from the build and the tests of this version; what is not measured yet is marked as open |
| Honest limits | Known gaps are documented and, where possible, held by a test that will flag the fix |
