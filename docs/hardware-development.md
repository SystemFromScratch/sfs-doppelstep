# Doppelstep hardware development

Doppelstep runs today on two boards that were designed for other purposes: the
[B-G431B-ESC1](hardware.md), an ST kit for drone motors, and the
[full-bridge board](hardware-full-bridge.md), the Stepper Motor FOC Closed-Loop Driver of
Awesome-SMT-Machine. Both are good starting points, and the next step is a **dedicated
Doppelstep PCB**.

This page collects what the firmware work and the review of both schematics showed, as
requirements for that PCB. Each item has an ID, so a schematic review can tick it off. An
item stays open until the new schematic covers it or a reason to leave it out is written next
to it.

!!! info "Source of the items"
    The items come from the firmware work for the full-bridge board (review of its
    schematic and of the DRV8412 datasheet, November 2026) and from the bring-up of the
    B-G431B-ESC1. Items marked *estimate* still need a measurement on the full-bridge board.
    Update this page after its bring-up.

## Power stage (DRV8412)

| ID | Requirement | Why |
|---|---|---|
| HW-01 | **Gate supply GVDD/VDD 12 V**, within 10.8–13.2 V. | On the full-bridge board the RY8411 sets 0.8 V × (1 + 118 kΩ / 10 kΩ) = 10.24 V (R6, R7), below the recommended range of the DRV8412. 12 V needs about 140 kΩ for R6. |
| HW-02 | **TVS on the 12 V rail above the rail voltage**, e.g. SMF13A instead of SMF10A. | The SMF10A (D1) has a standoff voltage of 10 V and starts to break down at about 11 V. The 10.24 V rail is already above its standoff, and at 12 V it would conduct. |
| HW-03 | **10 kΩ pull-downs on RESET_AB and RESET_CD.** | The firmware opens the bridges with RESET low (Hi-Z). Until the MCU drives the pins, during reset and in MCUboot, they float. TI recommends holding them low during power-up. |
| HW-04 | **OC_ADJ resistor from the table of the datasheet** (24–36 kΩ), chosen for the target motor current. | R31 = 47 kΩ ("5.8 A" on the schematic) is outside the table, so the cycle-by-cycle limit is unknown. The CBC limit cuts the current silently, without FAULT. |
| HW-05 | **FAULT and OTW on pins that stay free.** Consider FAULT on a TIM1 break input (BKIN/BKIN2), if the pin map allows it. | FAULT is read in software in every 50 µs period today (PB11 is not a break input). The DRV8412 already protects itself, so this is an option, not a must. OTW (PB0) blocks COMP4, which is fine because only the overvoltage comparator is used. |
| HW-06 | **Keep the four PWM inputs on TIM1 CH1..CH4** and the shunt amplifiers on the OPAMP pins as now. | The backend `sfs,stm32-full-bridge-power-stage` uses this map; the ADC trigger comes from the internal OC6. A different map is possible, but every change needs a change of the board files. |

## Current and voltage sensing

| ID | Requirement | Why |
|---|---|---|
| HW-07 | **Both shunts in the same position relative to their coil** (both in the "+" leg or both in the "−" leg). | Today the shunt of coil A is in leg A+ and that of coil B in leg B−, so the two readings have opposite signs. The firmware corrects it (`phase-b-sense-invert`), but one polarity is simpler to bring up and to check. |
| HW-08 | **Amplifier gain and shunt for the target motor.** Full scale about 2–3 × the rated coil current; a gain that settles in less than 1 µs. | 10 mΩ × 25.5 gives ±6.47 A for a 2 A motor: about 3 ADC counts per 10 mA. At a gain of 26.5 the internal OPAMP (13 MHz GBW) has about 0.5 MHz bandwidth and needs about 1.5 µs to settle. That sets the 2 µs sampling window (*estimate*) and limits the maximum duty cycle. |
| HW-09 | **VBUS on a comparator input** (COMP3 on PA0 today) and on an ADC1 channel. | The hardware overvoltage trip (COMP3 + DAC1 into BRK2) is the one hardware comparator the full-bridge board keeps ([Protection](protection.md#one-comparator-on-the-full-bridge-board)). |
| HW-10 | **Bus divider and NTC as on the full-bridge board** (10 kΩ / 100 kΩ; 10 kΩ NTC with B 3435 and 10 kΩ to GND), each on an ADC channel. | The firmware and the factory limits are set up for these values. The NTC on PA4 reaches only ADC2; the backend supports that. |

## Supply and protection of the input

| ID | Requirement | Why |
|---|---|---|
| HW-11 | **5 V rail sized for the load**: a regulator with a few hundred mA, or a buck. | The 75L05 gives about 100 mA. It feeds the WS2812B LEDs (tens of mA each at full white), the CAN transceiver (tens of mA when dominant) and the 3.3 V LDO. With eight LEDs at full brightness it is overloaded. |
| HW-12 | **TVS of the 24 V input with margin to the supply and coordinated with the overvoltage trip** (30 V in the firmware), e.g. SMAJ28A–SMAJ30A instead of SMAJ24A. | The SMAJ24A has a standoff voltage of 24 V and starts to break down at 26.7 V, little margin for a 24 V supply at its upper tolerance. The firmware switches the bridge off at 30 V. |
| HW-13 | **Bulk capacitance for the energy that returns while braking**, and a decision on reverse-polarity protection of the input. | A decelerating motor feeds energy back into the bus. A laboratory supply cannot take it, so the bus voltage rises until the overvoltage trip acts. |
| HW-14 | **A way to power the logic without 24 V** for flashing and bring-up: a jumper, or 3.3 V from the debug connector. | Today the MCU is supplied only from 24 V. Without it the DRV8412 holds FAULT active (undervoltage), which is expected and harmless, but flashing then needs the motor supply. |

## Bus and connectors

| ID | Requirement | Why |
|---|---|---|
| HW-15 | **DYNAMIXEL TTL bus as on the X-series servos**: half-duplex, one data line, a tri-state buffer with a direction pin, and the 3-pin connector of the X series. Option: RS-485 for longer cables. | Doppelstep answers as an XM430-W350. To share a cable with real servos, the board needs the same electrical bus. USART3 on J4 is a plain TTL UART today (single-wire is possible in the firmware). |
| HW-16 | **No 24 V on the supply pin of a bus shared with 12 V DYNAMIXEL servos.** | On the DYNAMIXEL cable the supply pin powers the servos. A Doppelstep board at 24 V must not feed it into a chain of 12 V servos. |
| HW-17 | **CAN only if a product needs it; if kept, the 120 Ω termination on a jumper.** | Doppelstep does not use CAN. On the full-bridge board R10 = 120 Ω is fitted on every board, while J6/J7 are made for a daisy chain: with several boards the bus has too many terminations. |
| HW-18 | **NRST on the debug connector**, preferably the standard 10-pin Cortex debug connector or a Tag-Connect footprint. | J3 has no NRST, so a probe cannot reset the MCU in hardware, for example when the firmware disables SWD pins or sleeps. |

## MCU and peripherals

| ID | Requirement | Why |
|---|---|---|
| HW-19 | **More flash**: a pin-compatible STM32G4 with 256 KiB or more, e.g. STM32G474CE or STM32G491CE (512 KiB, 48 pins). Check the pin and peripheral map (OPAMP, COMP, TIM1) against this board before choosing. | The full-bridge firmware needs about 86.4 KB of the 87.7 KB slot (*estimate*, GCC); MCUboot and the swap partitions take the rest of 128 KiB. Any new feature needs more room. |
| HW-20 | **Keep the internal OPAMPs, comparators and DACs** of the MCU for current sensing and trips. | A cheaper MCU without them (for example an STM32F103) would need external amplifiers and comparators. It also has no FPU, so the motor control would need the fixed-point backend ([Porting](porting.md)). |
| HW-21 | **Status LED: one LED is enough.** Either a plain GPIO LED, or one WS2812B with a 5 V level shifter on the data line (e.g. a 74AHCT1G125), or on a pin with an SPI MOSI or a timer channel with DMA. | The WS2812B at 5 V needs VIH = 0.7 × 5 V = 3.5 V, more than the 3.3 V of the MCU. PB3 is not an SPI MOSI pin, so Doppelstep drives it with its own TIM2 + DMA driver (1.3 KB of flash). A GPIO LED costs no flash beyond the GPIO driver. |
| HW-22 | **Encoder: magnetic angle sensor on SPI**, magnet on the motor shaft as on the full-bridge board; confirm the part (MA730 in the schematic, MA732 in the README of Awesome-SMT-Machine). | The firmware uses the MA730 for Present Position, lost steps and the position controller ([Encoder of the joint](encoder.md)). Another part needs another driver. |
| HW-23 | **A user button** (BTN1 on PA15 today). | Reserved for a local function, for example a factory reset of the ID and baud rate. |

## Bring-up and test

| ID | Requirement | Why |
|---|---|---|
| HW-24 | **Test points** for VBUS, 12 V, 5 V, 3.3 V, the four PWM inputs, RESET_AB/CD, FAULT, OTW and the two amplifier outputs, with a ground point next to each group. | The bring-up checklist measures these with an oscilloscope: resets before PWM, the sampling window, the current offsets. |
| HW-25 | **Room for a current probe** on one coil lead, or a 0 Ω link that can be replaced by a wire loop. | The dead time and the current ripple of the ESC1 could only be measured indirectly, through the ADC ([Measurements with MCUViewer](measurements.md)). |

## Lessons from the B-G431B-ESC1

These come from running Doppelstep on the ST kit and are already covered by the full-bridge
board. They are kept here as reasons for the choices above.

| Lesson | Consequence for the dedicated PCB |
|---|---|
| Three half-bridges force the shared-leg topology: the two coil currents add in leg W. | Two full H-bridges, one per coil (full-bridge topology of Stepper-C). |
| Discrete gate drivers with a large dead time (1200 ns set; 1.05 µs effective at 24 V) distort the current at low values. | A gate driver with a short integrated dead time, as the DRV8412 (5.5 ns typical). |
| The serial bus goes through the USB port of the on-board debugger. | A real bus connector on the board (HW-15). |
| No encoder: positions are the commanded ones, a lost step goes unnoticed. | An encoder on the motor shaft (HW-22). |

## Status

All items are **open** until the schematic of the dedicated Doppelstep PCB exists. The
full-bridge board itself is not changed: it stays the validation platform for the firmware.
