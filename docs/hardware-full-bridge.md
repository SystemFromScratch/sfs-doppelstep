# Full-bridge board (DRV8412)

The second board of Doppelstep, `sfs_doppelstep_full_bridge`, drives the stepper with two
full H-bridges in one gate driver, the TI DRV8412, instead of the three half-bridges of the
B-G431B-ESC1. Each coil has its own bridge, so no leg is shared, the current of a coil never
adds to the other, and the full bus voltage is across each coil. The MCU is the same
STM32G431CB, so the firmware above the power stage is unchanged.

!!! warning "Status"
    The firmware for this board is built in CI. Its power stage logic is tested on
    `native_sim` with the full-bridge topology, the encoder driver against an emulated MA730,
    the bit stream of the LED driver on the PC, and the use of the encoder in the application in
    the SIL tests with a simulated encoder. It has **not** run on a board yet: every value
    marked as an estimate below comes from the schematic and has to be confirmed in the
    bring-up.

## Origin of the board

The board is the **Stepper Motor FOC Closed-Loop Driver** of
[Awesome-SMT-Machine](https://github.com/baoqi-zhong/Awesome-SMT-Machine), the open-source
pick-and-place machine of [Ziyang Zhong](https://baoqi-zhong.github.io/) and
[Ziming Xian](https://github.com/ZimingXian). Ziming Xian designed the PCB, a 4-layer board
that mounts on the back of a NEMA 17 stepper motor; Ziyang Zhong did the software bring-up and
debugging. In their machine it runs their own firmware: a field-oriented
control (FOC) with torque, speed and angle loops, Kalman filtering, a zero-speed position
controller, and a CAN protocol that keeps several axes in step.

![The Stepper Motor FOC Closed-Loop Driver on a NEMA 17 motor, and its block diagram: 24 V bus, buck to 12 V, LDOs to 5 V and 3.3 V, STM32G431CBU6, DRV8412, CAN transceiver, WS2812B LEDs and MA730 magnetic encoder. Image: Ziyang Zhong and Ziming Xian, Awesome-SMT-Machine](assets/images/full-bridge/stepper-motor-foc-driver.png)

The authors agreed that Doppelstep references their project and welcomed fabricating the
open-source PCB for early-stage validation.

!!! note "Terms of use of the board"
    The board is for early-stage validation of Doppelstep. Do not use it directly for
    commercial purposes, such as a company demo, without discussing it first with both
    authors, Ziyang Zhong and Ziming Xian.

The board is **not the final hardware** of Doppelstep, but a good starting point: a compact, proven closed-loop stepper driver with an
encoder on the motor axis and the same MCU as the B-G431B-ESC1. Doppelstep replaces the
original firmware: it drives the motor with microstepping current control (Stepper-C in the
full-bridge topology), reads the encoder for the position of the joint, and answers as a
DYNAMIXEL servo on USART3 instead of CAN. Flashing Doppelstep overwrites the original firmware.

The description on this page follows the published schematic,
[Stepper-Motor-FOC-Driver-Schematic.pdf](https://github.com/baoqi-zhong/Awesome-SMT-Machine/blob/main/Hardware/Stepper-Motor-FOC-Driver/Stepper-Motor-FOC-Driver-Schematic.pdf)
(KiCad 8). The design files are in the
[Hardware/Stepper-Motor-FOC-Driver](https://github.com/baoqi-zhong/Awesome-SMT-Machine/tree/main/Hardware/Stepper-Motor-FOC-Driver)
folder of that repository, under the license of Awesome-SMT-Machine. Doppelstep contains none of the design files.
It contains only its own board definition, written from the schematic.

## The board

The table lists the parts of the schematic and what Doppelstep does with each of them.

| Part | Detail | In Doppelstep |
|---|---|---|
| Microcontroller | STM32G431CBU6 (Arm Cortex-M4F, 128 KiB flash, 32 KiB SRAM), 8 MHz crystal Y1, 170 MHz | Board `sfs_doppelstep_full_bridge` |
| Power stage | TI DRV8412: two full H-bridges, integrated dead time, cycle-by-cycle current limit (mode M3..M1 = 000), current limit set by R31 on OC_ADJ | Backend `sfs,stm32-full-bridge-power-stage`, TIM1 CH1..CH4 |
| Current sensing | 10 mΩ low-side shunts in leg A+ (R25) and leg B- (R15), OPAMP1/2 of the MCU in standalone mode with an external gain of 26.5 from the + input | Injected ADC conversions in the 20 kHz loop |
| Bus voltage, temperature | Divider R17/R18 on VBUS, NTC TH1 with R19 | ADC, hardware overvoltage trip COMP3 + DAC1 |
| Encoder | MPS MA730 (MA730GQ-Z, U3) magnetic angle sensor on SPI2, magnet on the motor shaft | Driver `sfs,ma730` (Zephyr sensor API), [Encoder of the joint](encoder.md) |
| Bus | USART3 on J4 (TTL, 33 Ω series resistors) | DYNAMIXEL Protocol 2.0, MCUboot serial recovery |
| CAN | FDCAN1 with a TJA1042T/3 transceiver (U4; SIT1042T/3 in the block diagram), NUP2105L protection, 120 Ω termination R10, on J6/J7 | Not used |
| Supply | 24 V on J6/J7 (6-pin, 24 V, GND, CAN_H and CAN_L for a daisy chain), 8 A polyfuse, SMAJ24A TVS diode; RY8411 buck (U2) to the 12 V gate supply of the DRV8412, 75L05 (U9) to 5 V, RS3236 (U8) to 3.3 V | Undervoltage and overvoltage limits of the motor node |
| Status LEDs | Eight WS2812B-2020 on PB3 | Driver `sfs,ws2812-stm32-tim` (Zephyr `led_strip` API, TIM2 CH2 with DMA); the STATUS LED is the first one |
| Button | BTN1 on PA15 | Defined in the board, not used by the application yet |
| Debug | SWD on J3 (no NRST on the connector) | OpenOCD, `support/openocd.cfg` |

The README of Awesome-SMT-Machine names the MA732, while the schematic and the block
diagram show the MA730. Doppelstep follows the schematic. Check the part on your board,
and read the datasheet before you fit an MA732.

The board definition is in `boards/sfs/sfs_doppelstep_full_bridge/`, the application nodes
(power stage, motor, servo) in `app/boards/sfs_doppelstep_full_bridge.overlay`.

```bash
west build --sysbuild -b sfs_doppelstep_full_bridge -s app
```

## Motor wiring

| Motor lead (17HS19-2004S1) | Coil end | Board output | Leg (TIM1) | Current sense |
|---|---|---|---|---|
| A+ | Coil A start | OUT_A (J1) | leg 0, PWM_A, PA8 (CH1) | R25 in GND_A → OPAMP1 |
| A- | Coil A end | OUT_B (J5) | leg 1, PWM_B, PA9 (CH2) | — |
| B+ | Coil B start | OUT_C (J8) | leg 2, PWM_C, PA10 (CH3) | — |
| B- | Coil B end | OUT_D (J9) | leg 3, PWM_D, PA11 (CH4) | R15 in GND_D → OPAMP2 |

This is the full-bridge topology of Stepper-C: phase A = leg 0 − leg 1, phase B = leg 2 − leg 3.

## How the backend drives the DRV8412

The backend is `drivers/power_stage/power_stage_stm32_full_bridge.c` (compatible
`sfs,stm32-full-bridge-power-stage`). It differs from the shared-leg backend of the ESC1 in
five points:

1. **One PWM input per half-bridge.** In mode 000 a PWM input high switches the high side of
   its leg on, low the low side; the DRV8412 inserts the dead time itself (5.5 ns typical). TIM1
   CH1..CH4 drive the four inputs without complementary outputs and without dead time
   (`dead-time-ns = <0>`). Unlike the shared-leg backend, a dead time of 0 does not refuse the
   outputs.
2. **The ADC trigger moves to channel 6.** CH4 drives leg B−, so the trigger of the injected
   conversions is the internal OC6 of TIM1 (`adc-trigger-channel = <6>`), at the top of the
   counter, where all four PWM inputs are low and both shunts carry their coil current.
3. **Outputs off means RESET_AB and RESET_CD low** (PC6, PA12). With one input per leg, the
   idle level of the timer (low) would close the low sides, not open the bridge. The backend
   asserts both resets before anything else at startup, in `output_enable(false)` and in the
   interrupt on every trip; it releases them only in `output_enable(true)`. Their low level
   also charges the bootstrap capacitors through the weak pull-downs of the DRV8412.
4. **FAULT and OTW.** FAULT (PB11) is read in every period while the outputs are on. When it
   is active, the backend opens both bridges and passes `POWER_STAGE_TRIP_DRIVER`, which the
   motor driver latches as `SSTEP_FAULT_HAL` (Hardware Error Status: Electrical Shock). The
   outputs stay refused until the MCU resets. That covers the one second TI recommends
   before clearing an overcurrent or overtemperature shutdown. While OTW (PB0) is active,
   `power_stage_get_temperature()` reports at least 125 °C, so the Temperature Limit of the
   emulated servo switches the torque off.
5. **Polarity per phase.** The shunt of phase A sits in leg A+, that of phase B in leg B−.
   At the sample, a positive coil A current flows up through R25 and a positive coil B current
   flows down through R15, so their readings have opposite signs. `current-sense-invert`
   turns both, and `phase-b-sense-invert` turns phase B back.

## Encoder (MA730)

`drivers/ma730/` (compatible `sfs,ma730`) reads the MA730 on SPI2 through the Zephyr SPI
driver, in mode 3 at 10 MHz. It offers the angle in two forms:

- the Zephyr sensor API: `SENSOR_CHAN_ROTATION`, in degrees;
- `<doppelstep/drivers/ma730.h>`: the raw angle (65536 counts per turn, 14 bits effective)
  and the magnetic field flags MGH/MGL.

`reverse` and `zero-offset` of the node are applied in software. The driver never writes
the registers of the MA730: they sit in non-volatile memory, rated for 1000 write cycles.

At startup the application checks once that the encoder answers and that the magnet is in
range (log `encoder: angle …/65536, field in range`). A missing or misplaced magnet is only
a warning, because the motor runs open loop without the encoder. The filter of the MA730 has a
cutoff of about 23 Hz, which is enough for the position of a joint and too slow for
commutation. The application uses the angle for Present Position, for lost steps and for
the position controller ([Encoder of the joint](encoder.md)).

The tests run the driver against an emulator of the MA730 on an emulated SPI bus
(`tests/drivers/ma730`): angle and register frames, field flags, the sensor API, reverse and
zero offset, and no write to the non-volatile memory.

## Status LED (WS2812B)

The STATUS LED of the firmware (on/off from the LED register, fast blink on an alarm) is the
first WS2812B of the chain, in red, at a low brightness (`CONFIG_SFS_STATUS_LED_BRIGHTNESS`,
default 32 of 255). The other seven stay dark. The application writes the chain only when
the state changes (`app/src/status_led.c`).

The driver `drivers/ws2812/ws2812_stm32_tim.c` (compatible `sfs,ws2812-stm32-tim`) belongs to
the Zephyr `led_strip` class. TIM2 CH2 on PB3 runs in PWM mode with a period of one bit
(1.25 µs, 213 ticks). On every update event, DMA1 channel 1 writes the compare value of the
next bit: 68 ticks (0.4 µs) for a 0, 136 (0.8 µs) for a 1. Two zero periods end the stream,
and then the chain latches after 300 µs. The DMA works in hardware, so the zero-latency
fast loop cannot stretch a bit. The CPU waits about 30 µs per pixel in the main thread.

**Why not Zephyr's own WS2812 drivers:**

- `worldsemi,ws2812-spi` needs an SPI MOSI pin, and PB3 is not one.
- `worldsemi,ws2812-gpio` exists only for nRF.
- `worldsemi,ws2812-uart` would work on PB3 (USART2 TX, inverted, 7 data bits at 2.5 Mbps),
  but it needs the asynchronous UART API, the DMA driver and the system work queue. Measured:
  **+10.4 KB** of flash, and the application no longer fits in the 86 KB slot.

The TIM2 driver adds about 1.3 KB together with the `led_strip` code and the status LED
module. Like a power stage backend, it programs its timer, its DMA channel and its DMAMUX
channel itself, so `timers2`, `dma1` and `dmamux1` stay disabled; `scripts/check-layers.sh`
allows LL calls in this one file. The bit stream (`ws2812_encode.h`) is tested on the PC
(`tests/drivers/ws2812`).

## Flash

| Build (clang, RTT; GCC in CI is about 7 % smaller) | Flash |
|---|---|
| Power stage, motor, servo, MCUboot hooks, RTT log | 85.3 KB |
| + MA730 encoder (Zephyr SPI and sensor) | + 4.4 KB |
| + WS2812 on TIM2 with DMA (this driver) | + 1.3 KB |
| + encoder tracking, lost steps, regulator ([Encoder of the joint](encoder.md)) | + 1.3 KB |
| = Full-bridge board | 92.4 KB clang, about 86.4 KB with GCC, of the 87.7 KB slot |
| (with `worldsemi,ws2812-uart` instead) | (+10.4 KB: does not fit) |

With the deferred log of Zephyr the GCC build took 87,268 B of the 87,712 B the slot leaves
for the image: 444 B to spare. `app/prj.conf` therefore builds every board with smaller
logging:

| Option (`app/prj.conf`) | Flash | RAM |
|---|---|---|
| `CONFIG_LOG_MODE_MINIMAL=y`: printk in the caller, no log thread, buffer or backends | − 9.8 KB | − 2.3 KB |
| `CONFIG_CBPRINTF_NANO=y`: small printf core (`%d %u %ld %x %c %s`, width) | − 0.7 KB | |
| `CONFIG_BOOT_BANNER=n`: `main.c` logs `Doppelstep <version> on <board>` instead | − 1.2 KB | |
| = Full-bridge board, GCC | 76,684 B, **11,028 B to spare** | 18,920 B of 32 KiB |
| with DXL2-C 2.0.0 and X430 1.2.0 (packets decoded in place) | 76,596 B, **11,116 B to spare** | 18,152 B of 32 KiB |

Minimal logging prints in the context of the caller, also in the control task. With a
debugger attached and the RTT buffer full, the RTT console waits up to
`CONFIG_RTT_TX_RETRY_CNT` x `CONFIG_RTT_TX_RETRY_DELAY_MS` for the host; both board
configurations set `CONFIG_RTT_TX_RETRY_CNT=0`, so the console drops the characters instead.
Log lines have no timestamp.

MCUboot does not read the encoder. The MA730 driver, as the sensor drivers of Zephyr, is built
only when the image turns the sensor API on (`CONFIG_SENSOR=y` in
`app/boards/sfs_doppelstep_full_bridge.conf`); MCUboot does not, although the node is in the
board devicetree. Pulled in, SPI and the sensor API (about 3 KB) did not fit in its 38 KiB
partition; without them MCUboot takes 37,148 B.

Reserve for later: `CONFIG_LTO=y` with `CONFIG_ISR_TABLES_LOCAL_DECLARATION=y` saves about
6.3 KB more, at the cost of harder debugging (inlined functions, renamed static symbols).
Read the real figures from the `FLASH:` lines of `build-ci/full-bridge.log`.

## Current and voltage scaling

| Quantity | Value | Source |
|---|---|---|
| Shunt | 10 mΩ | R15, R25 |
| Amplifier + input | 0.0623 V + 0.962 × Vshunt | 1 kΩ to the shunt, 51 kΩ to +3.3VADC, 51 kΩ to GND |
| Gain from the + input | 26.5 | 1 + 25.5 kΩ / 1 kΩ (R16, R30 over R12, R29) |
| Gain from the shunt | 25.5 (`current-sense-gain-milli = <25500>`) | label k = 25.5 on the schematic |
| Offset at 0 A | 1.65 V, 2048 counts (window 1850..2250) | |
| Current full scale | ±6.47 A | 1.65 V / (10 mΩ × 25.5) |
| Bus divider | 10 kΩ / (10 kΩ + 100 kΩ), 36.3 V full scale | R18, R17 |
| NTC | TDK NTCG103JF103FT1 (as on the ESC1), 10 kΩ to GND | TH1, R19 |

The current limit of the DRV8412 (R31 = 47 kΩ on OC_ADJ, about 5.8 A by the label of the
schematic) is inside the measuring range. The software trip of the motor node (3.5 A) acts first.

## Sampling window (estimate)

`sample-window-ns = <2000>`: the propagation delay of the DRV8412 and the settling of the
amplifiers before the trigger, the sampling of phase A and VBUS after it. The OPAMPs have a
gain bandwidth of 13 MHz, so at a gain of 26.5 their bandwidth is about 0.5 MHz and they settle
to 12 bits in about 1.5 µs. The window also keeps every high side off for 4 µs per period,
far more than the 50 ns the bootstrap needs. Measure it as on the ESC1
([Sampling window](hardware.md#sampling-window)) and lower it if the samples allow.

## Protection

The DRV8412 protects its own switches: a cycle-by-cycle current limit in every period, and an
overcurrent shutdown on a short (FAULT). The board therefore uses **only the hardware
overvoltage trip** (COMP3 on PA0 against DAC1 into BRK2), not the current comparators of the
ESC1. The [Protection](protection.md#one-comparator-on-the-full-bridge-board) page gives the
reasons. When the break fires, MOE goes to 0 and the four PWM inputs go low, so the low sides
short the coils until the interrupt asserts both resets, within one PWM period (50 µs).

## Bring-up checklist

Do these steps as on the ESC1 ([Bring-up checklist](hardware.md#bring-up-checklist)), with
a laboratory supply at 24 V and a current limit of 0.5 A at first:

1. **Resets before PWM.** Use an oscilloscope on RESET_AB and RESET_CD from power-on to the
   first Torque Enable. Both stay low until the enable, and both go low within 50 µs of
   Torque Enable 0 or a fault.
2. **Startup log.** It shows `TIM1: ... gate driver dead time 0 ns, ADC trigger CH6`,
   `current offsets A …, B …` near 2048, `hardware overvoltage trip 30000 mV: DAC level 3384`
   and no `<err>`.
3. **Polarity.** In voltage drive (`voltage-drive;` on the motor node), the measured current
   of each phase has the sign of its voltage. If phase A is wrong, change
   `current-sense-invert`. If only phase B is wrong, change `phase-b-sense-invert`.
4. **Sampling window.** Measure it at a duty cycle near 100 %, as on the ESC1.
5. **Self-test.** Run the self-test with the motor, with one coil open and with the coils swapped.
6. **Encoder and LED.** The log shows `encoder: angle …/65536, field in range`. Turn the
   shaft by hand: the angle follows. Write the LED register (65) over the bus: the first
   WS2812B lights red. Check the bit timing on PB3 with an oscilloscope (0.4 µs and 0.8 µs
   high in a 1.25 µs period).
7. **FAULT.** Short OUT_A to OUT_B through a switch at 12 V and a current limit of 3 A. The
   DRV8412 shuts down, the log shows the HAL fault, and Torque Enable is refused until a
   reset.

## Open points of the schematic

The points that came up while the firmware was written (gate supply 10.24 V, reset pins
without pull-downs, the 5 V regulator, the data level of the WS2812B, the CAN termination,
OC_ADJ and others) do not change the firmware. They are collected as requirements for the
dedicated Doppelstep PCB in [Hardware development](hardware-development.md). Check them on
this board during the bring-up.

## Acknowledgements

Many thanks to **Ziming Xian** for designing this board and to **Ziyang Zhong** for
bringing up its software, to both of them for publishing it with its schematic in
Awesome-SMT-Machine, and for agreeing that Doppelstep references their project. Thank you also for supporting the project. A good open design saves a new project months:
Doppelstep could start on a closed-loop board with an encoder right away, instead of
designing one first.

Have a look at their work as well: the pick-and-place machine, its CoreXY mechanics, the main control board and the
host software are all in
[Awesome-SMT-Machine](https://github.com/baoqi-zhong/Awesome-SMT-Machine).
