# Hardware checklist after the driver refactor

CI runs on `native_sim`, so it cannot see the real interrupt, timer, ADC or UART. Run these checks
on a B-G431B-ESC1 after a change to the drivers or the tasks, and write down the results. They need
no motor unless stated, and the gate drivers stay off except in check 5, which measures the dead
time on the bridge without a motor.

To compare timing, first measure the fast loop on the firmware before the change.

| Value | Before | After |
|---|---|---|
| `fast loop max` [cycles], simulated motor, during a move | 8070 (47.5 µs, the model before Stepper-Lab 9fc2eef) | 6341 (37.3 µs) |

## A: firmware without a motor

Build and flash:

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build
west flash -d build -r openocd
```

Then read the log with `west rtt -d build -r openocd`.

| # | Check | Expected |
|---|---|---|
| 1 | Startup log | No `<err>`. Offsets about 2530 counts (inside 2300..2800), `hardware overcurrent trip 10000 mA: DAC3 …` near 180, `hardware overvoltage trip 30000 mV: DAC1 3583`, `TIM1: ... dead time 1200 ns (204 ticks)`, the line `x430_servo: DYNAMIXEL` |
| 2 | STATUS LED | Slow blink: outputs off |
| 2a | Hardware Error Status over 10 power cycles, the first with erased NVS | 0 every time, LED slow blink. No false fault from the shared-leg check at startup |
| 2b | Realtime Tick over a minute, torque on at rest | Rises steadily: no watchdog reset. The startup log has `watchdog 500 ms` and no `reset by the watchdog` |
| 3 | `tools/sim/dxl_smoke_test.py` on the ST-LINK COM port, 1 Mbps | Ping answers model 1020 |
| 4 | DYNAMIXEL Wizard scan | Finds one XM430-W350 at ID 1, 1 Mbps |
| 5 | Build with `-DCONFIG_SSTEP_MOTOR_SELFTEST=n` (the self-test needs both coils). Torque Enable, no motor (a 10 Ω power resistor between U and W if a leg shows no edges), supply at 12 V with a 200 mA current limit | Accepted, no fault. Supply current stays small. Low-side VGS on the oscilloscope: the off gate stays well under VGS(th) at every edge ([Dead time, step 6](dead-time.md#step-6-check-on-the-board)). Torque Disable afterwards |
| 6 | Present Input Voltage with a supply connected | Agrees with a multimeter |
| 7 | Write Baud Rate 2 (115200 bps), then reconnect at that speed | Answers at the new speed. `native_sim` cannot test this |
| 8 | Power cycle after step 7 | Keeps the baud rate, so NVS works. Set it back to 3 afterwards |

Only check 5 switches the outputs on, without a motor. A real trip is checked in parts C and E.

## B: simulated motor on the board

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay
west flash -d build -r openocd
```

| # | Check | Expected |
|---|---|---|
| 9 | Startup log | `SIMULATED MOTOR`, no `<err>` |
| 10 | Torque Enable, Goal Position 3072 in the Wizard | Rotor of the model follows, lag well under 90 electrical degrees |
| 11 | STATUS LED during step 10 | On |
| 12 | PWM pins with an oscilloscope, or the MOE bit of `TIM1_BDTR` in the debugger | Gate outputs stay at their idle level, MOE = 0 |
| 13 | `fast loop max` in the log | Enter it in the table above. It must stay well under one PWM period, 50 µs. A rise over the old value should be small: a few tens of cycles for the extra API call. `overruns` stays 0; a fast loop that overruns switches the outputs off and reports Electrical Shock Error |
| 14 | Spam Goal Position from the Wizard or a script for a minute | Every packet answered, no `<err>`, `fast loop max` stable |

## C: with a motor, after the dead time is measured

Follow the [bring-up checklist](hardware.md#bring-up-checklist) first; its step 6 forces the trips, which confirms the
fault path through the interrupt. Checks 15 and 16 concern a finding from the simulation.

| # | Check | Expected |
|---|---|---|
| 15 | Phase current at rest with a current probe, at the 0.5 A hold current and at several rotor positions | Records the current ripple. In the model the current loop toggles by one ADC step (29.4 mA, about 570 mV of phase voltage) |
| 16 | Vibration and noise of the motor at rest, at the same positions | Records whether the motor audibly or measurably vibrates around 125 Hz at 0.5 A |

If checks 15 and 16 show a real problem, consider oversampling in the ADC, a lower current loop
bandwidth (`current-bandwidth-hz`) or a higher measurement gain. The model predicts the effect, but
real friction and ADC noise may reduce it or remove it, so measure before changing the control.

## D: firmware update over USART2

MCUboot serial recovery ([Firmware update](firmware-update.md)). Flash with the debug probe first
(task **Zephyr: Flash**), at 1 Mbps unless a check says otherwise.

| # | Check | Expected |
|---|---|---|
| 17 | Startup log after the probe flash | The application starts, so MCUboot accepted the signature |
| 18 | VS Code configuration **Debug: OpenOCD on host** | Stops at `main` of the application |
| 19 | Write a test value to Homing Offset (20), read it back | The value |
| 20 | Reboot, then an SMP request repeated until answered, then `image upload` and `os reset` of an image with another boot banner | Upload completes, the new banner in the RTT log |
| 21 | Baud Rate 1 (57600), Reboot | Complete status packet without error; the application answers again at 57600 after about 0.5 s plus startup |
| 22 | Upload interrupted halfway (unplug USB), power on | The application does not start; a second upload succeeds and the application starts |
| 23 | Read Homing Offset (20) | The value of check 19: the storage partition survived |

## E: protection

The PC tests check the logic of every protection ([Protection](protection.md)); these checks
cover the analog side. Use the real-motor build with the self-test on, a laboratory supply with
a current limit, and the RTT log open.

| # | Check | Expected |
|---|---|---|
| 24 | USB only, no supply: Torque Enable | Result Fail; Hardware Error Status 0x01 (Input Voltage); log `enable refused: VBUS … mV outside 10000..28000 mV`; nothing energised |
| 25 | Supply 12 V (200 mA limit), no motor: Torque Enable | Accepted at once, then Torque Enable reads 0 again within 0.5 s; Hardware Error Status 0x10; log `selftest: no current (phase open, …), phase A`. Reboot clears it |
| 26 | Motor connected, 12 V (1 A limit): Torque Enable | Accepted; log `selftest passed`; the shaft moves by at most one full step and back. Supply current during the test well under 1 A |
| 27 | One coil swapped onto U and V (A+ on U, A− on V): Torque Enable | Result Fail; log `selftest: current in the other phase (phase on the wrong legs)`. Wire it back |
| 28 | Build with `hw-trip-vbus-mv = <11000>`, supply 12 V | Right after the start: Hardware Error Status 0x01 without a Torque Enable, Torque Enable refused. Restore 30000 |
| 29 | Build with `hw-trip-current-milliamp = <1000>`, motor connected, 12 V (2 A limit): Torque Enable | The self-test passes (600 mA), then the run current trips the break: Hardware Error Status 0x10, the outputs off within the break filter (oscilloscope on a gate: no further pulse). Restore 10000 |
| 30 | Oscilloscope on a low-side gate during normal moves at 24 V, 1.5 A | No unexpected break (no Hardware Error) over a few minutes of moves: the 0.47 µs break filter rejects the switching spikes |
