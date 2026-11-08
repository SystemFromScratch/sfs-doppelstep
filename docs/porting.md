# Porting to a new MCU family

To move Doppelstep to another MCU family, you write one `power_stage` backend and describe the
board. The motor driver, the X430 driver and the application do not change. Within a family that
already has a backend, only the devicetree and the board configuration change.

## What a port needs

| Item | Where |
|---|---|
| A backend of the `power_stage` class, with its binding | `drivers/power_stage/`, `dts/bindings/sfs/` |
| The board, unless Zephyr already has it | `boards/` |
| The nodes of the stage, the motor and the servo | `app/boards/<board>.overlay` |
| Zephyr options of the board: FPU or not, console, flash writes | `app/boards/<board>.conf` |

Copy `drivers/power_stage/power_stage_stm32.c` as the starting point, and use the fake backend in
`tests/drivers/common/` as the shortest example of the API.

## Backend checklist

- [ ] **The fast callback makes no kernel calls.** It runs at the PWM rate in your interrupt.
  Choose the interrupt type in the backend: zero-latency where the core has it, otherwise the
  highest normal priority.
- [ ] **Sampling instant.** With low-side shunts, sample when the low-side switches conduct: at the
  center of a center-aligned period, triggered by the timer in hardware.
- [ ] **Offset calibration** runs with the outputs off and blocks only the caller.
- [ ] **Polarity.** `adc_i_span` follows the gain sign; `current-sense-invert` corrects the
  result of bring-up step 3.
- [ ] **`outputs_allowed`** is false until the dead time is configured, and `output_enable` then
  returns `-EPERM`.
- [ ] **Outputs start off.** Drive the idle levels before the pins connect to the timer.
- [ ] **`aux_trip`** covers a leg the sample does not show and stays false until calibration has
  succeeded: with an unknown offset a false trip would latch a fault at every start. Without such a
  measurement, leave `set_aux_trip` out and remove `aux-trip` from the motor node, because the motor
  driver refuses to start when the node asks for a check the stage cannot do.
- [ ] **`get_stats`** is optional: without a cycle counter, leave `fast_cycles_max` at 0.
- [ ] `pwm-frequency` is an integer multiple of `slow-loop-hz`.
- [ ] Add the board build to `scripts/ci.sh` and extend the allowed file list of
  `scripts/check-layers.sh` with the new backend.

## What the code above the backend does not assume

| Feature | Rule |
|---|---|
| Zero-latency interrupts | The contract is "no kernel calls in the fast callback" |
| FPU | Without an FPU, Kconfig selects the Stepper-C fixed-point backend. CI runs the motion tests on it |
| Cycle counter | `fast_cycles_max` is optional |
| On-chip amplifiers | They belong to the backend and the board |
| Simulated motor on the board | The plant model uses floating point, so it needs an FPU or a PC |

## Example: TI MSPM0G3519

This is the planned next family. The figures below come from the datasheet page and from Zephyr
v4.5.0, not from a built port.

| Feature | STM32G431 | MSPM0G3519 | Consequence |
|---|---|---|---|
| Core | Cortex-M4F, 170 MHz | Cortex-M0+, 80 MHz | Half the cycles per PWM period |
| Zero-latency interrupts | Yes | No: Zephyr needs BASEPRI | A normal high-priority interrupt, subject to the kernel's interrupt locks |
| FPU | Yes | No | Fixed-point backend |
| Cycle counter (DWT) | Yes | No | No `fast_cycles_max`, or another timer |
| Motor timers | TIM1 | Two advanced timers with dead band and complementary outputs | New backend |
| ADC | ADC1, ADC2, injected | Two 12-bit ADCs, simultaneous sampling | New backend |
| On-chip amplifiers | OPAMP1..3 | Not listed by TI for this part | External amplifiers on the board |

Zephyr v4.5.0 already has `mspm0g3519.dtsi` and the board `lp_mspm0g3519`.

### Time budget

The main risk is time. At 20 kHz and 80 MHz, one PWM period has 4000 cycles and one millisecond
has 80000. Stepper-C (commit `3ef0f7e`, just before v1.2.0) was compiled for Cortex-M0+ with GCC
15.2, fixed-point backend, shared-leg topology, and run in a Cortex-M0 emulator with an M0+ cycle
model:

| Build | Image | Fast loop, max | CPU without stall detection | CPU with stall detection |
|---|---|---|---|---|
| `-Os` | 12.6 KB | 2305 cycles | 64 % | 102 % |
| `-O2` | 16.5 KB | 1516 cycles | 44 % | 68 % |

CPU figures are the worst case: sustained voltage limiting and a status read every millisecond.

!!! warning "These are estimates"
    The model has no flash wait states, no Zephyr interrupt entry and no ADC register reads, so real
    load is higher. Measure on the part before you rely on these numbers.

What to do with them:

- Set `CONFIG_SSTEP_OPTIMIZE_SPEED=y`, which builds Stepper-C with `-O2`. With 512 KB of flash,
  4 KB is a small price.
- With stall detection on, `sstep_get_status()` costs about 9000 cycles. Raise
  `CONFIG_SFS_STATUS_PERIOD_MS`: 10 ms saves about 10 percentage points.
- If the budget is still short, lower the fast-loop rate, which Stepper-C accepts as an integer
  fraction of the PWM rate, or move the fast path to SRAM with Zephyr's code relocation.

The fast loop itself needs no 64-bit arithmetic: the fixed-point backend uses 32-bit
multiplications there. 64-bit helpers appear in the slow loop.
