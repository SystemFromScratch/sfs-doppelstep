# Hardware checklist after the driver refactor

CI runs on `native_sim`, so it cannot see the real interrupt, timer, ADC or UART. Run these checks
on a B-G431B-ESC1 after a change to the drivers or the tasks, and write down the results. They need
no motor unless stated, and the gate drivers stay off.

To compare timing, first measure the fast loop on the firmware before the change.

| Value | Before | After |
|---|---|---|
| `fast loop max` [cycles], simulated motor | | |

## A: firmware without a motor

Build and flash:

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build
west flash -d build -r openocd
```

Then read the log with `west rtt -d build -r openocd`.

| # | Check | Expected |
|---|---|---|
| 1 | Startup log | No `<err>`. Offsets near 2048 counts, a warning that `dead-time-ns` is 0, the line `x430_servo: DYNAMIXEL` |
| 2 | STATUS LED | Slow blink: outputs off |
| 2a | Hardware Error Status over 10 power cycles, the first with erased NVS | 0 every time, LED slow blink. No false fault from the shared-leg check at startup |
| 3 | `tools/sim/dxl_smoke_test.py` on the ST-LINK COM port, 1 Mbps | Ping answers model 1020 |
| 4 | DYNAMIXEL Wizard scan | Finds one XM430-W350 at ID 1, 1 Mbps |
| 5 | Torque Enable | Result Fail: outputs are refused while `dead-time-ns` is 0. No fault, LED unchanged |
| 6 | Present Input Voltage with a supply connected | Agrees with a multimeter |
| 7 | Write Baud Rate 2 (115200 bps), then reconnect at that speed | Answers at the new speed. `native_sim` cannot test this |
| 8 | Power cycle after step 7 | Keeps the baud rate, so NVS works. Set it back to 3 afterwards |

These checks do not need the outputs. A real trip is checked in part C.

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
| 13 | `fast loop max` in the log | Enter it in the table above. It must stay well under one PWM period, 50 µs. A rise over the old value should be small: a few tens of cycles for the extra API call |
| 14 | Spam Goal Position from the Wizard or a script for a minute | Every packet answered, no `<err>`, `fast loop max` stable |

## C: with a motor, after the dead time is set

Follow the bring-up checklist in the README first; its step 6 forces the trips, which confirms the
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
