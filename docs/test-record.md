# Test record

What was tested for release 0.2.0, on the PC and on the board, and with which result. The
procedures are in [Tests and CI](testing.md) and the [hardware checklist](hardware-checklist.md);
this page records the outcome.

## Setup

| Item | Value |
|---|---|
| Board | STMicroelectronics B-G431B-ESC1 (STM32G431CB), MCUboot and the signed application |
| Motor | 17HS19-2004S1 (NEMA 17, 2 A, 1.6 Ω, 3 mH), wired with the shared leg W ([Hardware](hardware.md#motor-wiring)), shaft free, motor fixed |
| Supply | Laboratory supply: 12 V with 1 A and 2 A current limit, then 24 V with 2 A |
| Host | Windows PC, ST-LINK Virtual COM Port at 1 Mbps, DynamixelSDK 4.1.0 (the library under the ROS 2 `dynamixel_hardware_interface`) and the bench GUI |
| Log | RTT through the ST-LINK |

## PC tests (CI of the release)

`scripts/ci.sh` in the environment image, with Stepper-C 1.4.0:

| Gate | Result |
|---|---|
| Layer check | Pass |
| Board builds, no warnings | Real motor 77 420 B of the 87 712 B slot, simulated motor 84 100 B; MCUboot 37 148 B |
| ztest suites (`native_sim`) | 77 of 77 in 6 configurations |
| SIL and integration tests | 28 pass, 1 expected failure (lost synchronism: no stall detection) |
| Motion tests on the fixed-point backend | 8 of 8 |
| GUI host tests | 44 pass, 1 skipped |

## On the board with the motor, 12 V

Development firmware before the release (Stepper-C from `main` with the sampling window and
the current slew; the self-test of the motor driver itself, which Stepper-C 1.3 took over).

| Test | Result |
|---|---|
| Wiring against schematic MB1419 | Coil A on U and W, coil B on V and W, as in [Hardware](hardware.md#motor-wiring) |
| Self-test at Torque Enable | Pass, 44 ms |
| Moves of ±90° at 0.5 A | Reached; Hardware Error 0, VBUS 12.0 V; the shaft followed |
| Torque Enable at 1 A and 1.5 A | First an undervoltage trip: a duty cycle near 100 % corrupted the bus voltage sample. Fixed with the sampling window (`sample-window-ns`) and the current slew (`current-slew-ma-per-ms`); then no trip at 1 A or 2 A supply limit |
| 20 GUI test scenarios, 3 runs | 60 of 60 |
| Run current scenario alone | 10 of 10 |
| Reboot and Torque Enable with the self-test | 10 of 10 |
| Hardware overvoltage comparator, level 11 V on a 12 V supply | Input Voltage Error right after the start, Torque Enable refused |
| Hardware overcurrent comparators, DAC level swept with the debugger, outputs off | Switch at DAC code 159 (U, V) and 161 (W); with the 10 A level the break acts at about 9.9 A (U, V) and 8.9 A (W) |
| 2 minutes of moves at 46 rpm and 1.5 A (123 moves) | No fault, VBUS at least 11.9 V, bridge 39 → 48 °C |

## On the board with the motor, 24 V

Release firmware 0.2.0 with Stepper-C 1.4.0, 2 A current limit.

### Protection scenarios

| Scenario | Torque Enable | Result |
|---|---|---|
| No supply, USB only (bus 4.7 V) | Result Fail in 2.0 ms | Hardware Error 0x01 Input Voltage; log `enable refused: VBUS 4679 mV outside 10000..28000 mV`; nothing switched on |
| One coil disconnected | OK in 1.9 ms, off again | 0x10 Electrical Shock; `no current (phase open, …), phase A`, 29 mA at the largest duty |
| No motor | OK in 1.8 ms, off again | 0x10 Electrical Shock; `no current (phase open, …), phase A`, 0 mA |
| Coil A between U and V, coil B open (check 27) | OK in 1.9 ms, off again | 0x10 Electrical Shock; `current in the other phase (phase on the wrong legs), phase A`, 646 mA, 0.1 s after the start of the test |
| The two coils swapped, common wire on W | OK in 1.9 ms | Self-test passes, 5 of 5 with the rotor a full step away from the first tested phase; the motor turns the other way (a wiring without an encoder cannot show it) |

The leg-sum check of the motor driver (iU + iV + iW = 0) did not fail in the scenarios: the
sum stayed at 2 to 4 counts against the limit of 5.

### Normal operation

| Test | Result |
|---|---|
| Self-test, rotor a full step away from the first tested phase | 10 of 10 (with Stepper-C 1.4.0), before that 15 of 15 on the same algorithm; leg sum up to 17 counts in single samples, never more than 2 periods in a row above the limit |
| Torque Enable answer with DynamixelSDK | 1.8 to 2.1 ms; Torque Enable reads 1 at once |
| Goal Position 2 ms after Torque Enable, during the self-test | Executed when the test passed (Present Position = goal after 1 s) |
| ros2_control loop: Sync Read and Sync Write at 100 Hz for 3 s, sine goal | 300 cycles, no failed Sync Read, tracking error at most 11 pulses |
| Moves of ±90° at 1.5 A | Reached; Hardware Error 0, VBUS 24.0 V, 41 °C |

## Problems found by these tests

| Found | Cause | Fixed in |
|---|---|---|
| Undervoltage trip at Torque Enable, 12 V, 1 A and above | Duty cycle near 100 % in the samples of the low-side shunts | Sampling window and current slew (Stepper-C 1.3.0, power stage `sample-window-ns`) |
| Present Current jumped by an ADC step and read 30 to 60 mA with the torque off | Single samples and offset drift | Averaged over 8 ms, 0 with the torque off |
| Torque Enable answered after the self-test: DynamixelSDK timed out (CI) | DynamixelSDK waits about 34 ms, the test took 44 ms | Torque Enable answered at once, the self-test ends in the slow loop ([Protection](protection.md#self-test-of-the-bridge-and-the-motor)) |
| Self-test reported a coil on the wrong legs with the coils swapped, rotor a full step away | Stepper-C 1.3.0 judged one sample while the rotor still moved | Stepper-C 1.4.0: every check waits for the rotor to come to rest |
| Leg-sum check failed once in 15 tests | One disturbed sample passed an 8-period filter | The check counts only a sum above the limit for 2 ms in a row |
| The Stepper-C options of Kconfig did not reach the library | Wrong CMake variable in the glue | `COMPILE_DEFINITIONS` of `sstep.cmake` |
| SIL load-angle test failed now and then, in CI and once in the release pipeline (45 instead of 50 ± 4 deg) | It took one log sample: the log gives the lag in whole microsteps (5.6 deg), and the rotor of the model vibrates by a fraction of one at rest; it also waited in host time, which a loaded host stretches | The test averages the samples of a window in the time of the firmware |

## Open

- **Current ripple and vibration at rest** (checks 15 and 16): need a current probe and an
  oscilloscope, and listening to the motor at several rotor positions.
- **Dead time on the oscilloscope** (bring-up step 1) and **switching spikes at 24 V**
  (check 30): 1200 ns comes from the ngspice sweep. No fault and no hot switch at 24 V and
  1.5 A.
- **Hardware overcurrent with a low trip level** (check 29 as written): the comparators were
  checked by sweeping the DAC level instead; a level of 1 A trips at rest (comparator offset).
- **NTC against a thermometer** and the **fast-loop time of the real-motor build**.
- Once, in the first run of the GUI scenarios at 12 V, Torque Enable was refused in the Bus
  Watchdog scenario; not again in the 13 runs after it, and the RTT log lost the reason.
