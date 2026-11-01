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

The build result is `build/zephyr/zephyr.elf`; flashing and debugging work with
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

## Repository layout

Everything that belongs to Zephyr lives in `apps/`; host tools live in
`tools/`, the documentation in `docs/`.

```text
doppelstep/
├── west.yml                     west manifest (Zephyr v4.5.0, libraries)
├── apps/                        Zephyr: BOARD_ROOT and DTS_ROOT of the applications
│   ├── doppelstep/              firmware application (west build -s apps/doppelstep)
│   ├── boards/st/b_g431b_esc1/  out-of-tree board
│   ├── dts/bindings/sfs/        devicetree bindings
│   └── modules/                 Zephyr module glue of Stepper-C and the X430 emulator
├── tools/
│   ├── gui/                     host GUI (planned)
│   └── docs/                    PDF manual builder, MkDocs hooks, cover page
├── docs/                        this documentation (MkDocs)
├── scripts/                     west-init.sh (container), host/ GDB servers (Windows)
├── .vscode/                     tasks for Windows and Linux, Cortex-Debug launch configurations
├── .devcontainer/               Dev Container configurations
├── Dockerfile, compose.yaml     Zephyr toolchain image
├── mkdocs.yml, requirements.txt documentation and host Python tools
└── README.md
```
