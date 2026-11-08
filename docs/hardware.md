# Hardware and bring-up

Doppelstep runs on the STMicroelectronics **B-G431B-ESC1** Discovery kit, a small
electronic speed controller (ESC) board for drone motors, with a detachable debugger. This page describes
the board, how the stepper motor is wired to it, how the firmware scales the measurements,
and the steps to bring a new setup up safely.

## The board

| Part | Detail |
|---|---|
| Microcontroller | STM32G431CBU6: Arm Cortex-M4F, 170 MHz, 128 KiB flash, 32 KiB SRAM |
| Power stage | L6387 gate drivers, STL180N6F7 MOSFETs, three half-bridges (legs U, V, W) |
| Current sensing | Three 3 mΩ low-side shunts, amplified by the internal OPAMPs of the MCU |
| Debugger | ST-LINK/V2-1 on a detachable part of the board, with a Virtual COM Port |
| Pinout | UM2516 Rev 4, Table 4 (`boards/st/b_g431b_esc1/doc/index.rst`) |

The board definition is part of this repository (`boards/st/b_g431b_esc1/`), because Zephyr
4.5 does not include it: 170 MHz from the 8 MHz crystal, the pinout of the user manual, and
the flash runners OpenOCD (default), J-Link, STM32CubeProgrammer and pyOCD.

## Motor wiring

The board has three half-bridges, while a two-phase stepper has two coils. Doppelstep uses
the **shared-leg topology** of Stepper-C: each coil sits between its own leg and the common
leg W.

| Motor | ESC1 output |
|---|---|
| Phase A (coil A+ / A−) | U / W |
| Phase B (coil B+ / B−) | V / W |

W carries the sum of both coil currents, iA + iB. The firmware measures its shunt too and
switches the outputs off when |iW| exceeds the trip current (`aux-trip` of the motor node).

The tested motor is a **17HS19-2004S1** (NEMA 17, 2.0 A rated, 200 steps per revolution).
Another motor needs its values in the motor node ([Configuration](configuration.md)).

## Current and voltage scaling

The scaling comes from the power stage node of the devicetree
(`app/boards/b_g431b_esc1.overlay`):

| Quantity | Value |
|---|---|
| Shunt | 3 mΩ |
| Amplifier gain | −64/7 ≈ −9.14 (OPAMP PGA 16 with VINM0 as the bias input, as in ST MCSDK, NuttX and SimpleFOC) |
| Current full scale | ±60.2 A with a 3.3 V reference, about 29 mA per ADC count |
| VBUS divider | 18 kΩ / (18 kΩ + 169 kΩ) |
| VBUS full scale | 34.3 V |

## The DYNAMIXEL bus

The bus is **USART2** (PB3 TX, PB4 RX), for now through the ST-LINK Virtual COM Port of the
debugger, at 1 Mbps. A DYNAMIXEL X430-T has no direction pin either: for a TTL half-duplex
bus later, USART2 can run in single-wire mode (`single-wire;` on `&usart2`, TX pin only).
Because USART2 carries the bus, the console and the log use SEGGER RTT through the debug
probe.

## Bring-up checklist

Follow these steps in order on a new board or after a change to the power stage. They follow
the integration guide of Stepper-C.

1. **Dead time (required).** Enter the value of the B-G431B-ESC1 MCSDK (Motor Control
   Workbench) project in `dead-time-ns` of `app/boards/b_g431b_esc1.overlay`. While it is 0
   the firmware runs, but never enables the gate drivers: Torque Enable answers Result Fail
   and the log says "enable refused". Check the gate driver input polarity (TIM1 CHx/CHxN
   active high) against the same project.
2. **Outputs off.** The RTT log shows the current offsets (about 2048 counts) and the ADC
   readings. Compare Present Input Voltage with a multimeter.
3. **Polarity.** Set `voltage-drive;` and a low `run-current-milliamp` in the motor node.
   After Torque Enable the measured phase A current must be positive. If not, set
   `current-sense-invert;` in the power stage node.
4. **Current loop.** Back to current drive with a low run current: check that the current
   settles at the run current.
5. **Motion.** Small Goal Position steps (DYNAMIXEL Wizard or the [GUI](gui/index.md)), then
   the OpenMANIPULATOR-X profile.
6. **Protection.** Lower `trip-current-milliamp` and `vbus-max-mv`, then check the trips and
   the Hardware Error Status.

After a change to the drivers or the threads, also run the
[hardware checklist](hardware-checklist.md): it covers what the PC tests cannot see.
