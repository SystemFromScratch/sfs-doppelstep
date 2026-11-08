# Doppelstep

> *Your stepper, seen as a smart servo.*

Doppelstep is a Zephyr RTOS firmware for the STMicroelectronics
**B-G431B-ESC1** Discovery kit: a two-phase stepper motor (17HS19-2004S1) driven
in closed-loop microstepping current control by
[Stepper-C](https://github.com/AntonVynohradov/Stepper-C), seen by the host as a
**DYNAMIXEL XM430-W350** over Protocol 2.0 through the
[DYNAMIXEL X430 Control Table Emulator](https://github.com/AntonVynohradov/DYNAMIXEL-X430-Control-Table-Emulator).
Target host: ROS 2 `dynamixel_hardware_interface` as used by
[OpenMANIPULATOR-X](https://github.com/ROBOTIS-GIT/open_manipulator).

The build result is MCUboot and the signed application (`build/app/zephyr/`, see
[Firmware update](firmware-update.md)); flashing and debugging work with
OpenOCD (on-board ST-LINK/V2-1) and SEGGER J-Link, through `west` and VS Code
Cortex-Debug. Hardware details, the DYNAMIXEL register mapping, the
configuration options and the bring-up checklist are in the repository
`README.md`.

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

Layers, rules and the path of a command: [Architecture](architecture.md). For another
MCU family: [Porting to a new MCU family](porting.md).

## Repository layout

The repository is a Zephyr module with one application. Host tools live in `tools/`, the
documentation in `docs/`.

```text
doppelstep/
├── west.yml                     west manifest (Zephyr v4.5.0, libraries)
├── zephyr/module.yml            the repository is a Zephyr module
├── app/                         firmware application (west build -s app)
├── drivers/                     power_stage, motor, x430
├── include/doppelstep/drivers/  public driver APIs
├── boards/st/b_g431b_esc1/      out-of-tree board
├── dts/bindings/                devicetree bindings
├── modules/                     build glue of the libraries
├── tests/                       ztest suites, SIL and integration tests
├── tools/
│   ├── gui/                     host GUI (planned)
│   ├── sim/                     native_sim launcher, Protocol 2.0 smoke test
│   └── docs/                    PDF manual builder, MkDocs hooks, cover page
├── docs/                        this documentation (MkDocs)
├── scripts/                     CI, tests, layer check, west-init.sh, host/ GDB servers
├── .vscode/                     tasks for Windows and Linux, Cortex-Debug launch configurations
├── .devcontainer/               Dev Container configurations
├── Dockerfile, compose.yaml     Zephyr toolchain image
├── mkdocs.yml, requirements.txt documentation and host Python tools
└── README.md
```
