# Changelog

All notable changes to Doppelstep are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The version of a release is its
`vX.Y.Z` tag; `app/VERSION` holds the same number, and the release pipeline checks both
([Development environment](docs/development.md#releasing)).

## [Unreleased]

### Added

- **Fast-loop overrun protection.** When the fast loop does not finish within its PWM period,
  the STM32 backend switches the outputs off in the interrupt and reports
  `POWER_STAGE_TRIP_OVERRUN`; the motor faults (`SSTEP_FAULT_HAL`, Electrical Shock Error)
  and the bus keeps answering. Before, an overrunning fast loop kept the interrupt busy for
  good and stopped every thread, the bus included. `power_stage_stats.overruns` counts them,
  and the simulated-motor log prints the count.
- **Hardware watchdog** (IWDG, 500 ms): `main()` feeds it only when the control and the
  communication task have both run since the last check (`app/src/supervisor.c`). A fast loop
  that leaves the threads no time, or a hung task, resets the MCU, and the outputs go off. The
  log tells a watchdog reset; a debugger halt pauses the watchdog.
- [Dead time of the power stage](docs/dead-time.md): the method behind the dead time, step by
  step, with excerpts of schematic MB1419, a hand estimate, the ngspice sweep and its results.
  `tools/power/deadtime_sweep.py` repeats the sweep, the figures and the schematic excerpts.
- **`output/` with versioned images:** every board build of the VS Code tasks copies the
  merged HEX (first flashing), the signed update file for MCUboot, the signed HEX and the ELF
  into `output/` (git-ignored), named `doppelstep-<git describe>-b_g431b_esc1-…`
  (`scripts/export_output.py`). The GUI preselects the newest signed image there.
- **GUI torque state:** the Motion tab shows the torque state from the live registers (green
  *TORQUE ON*, grey *Torque off*); Torque ON and Apply are disabled while the torque is on.

### Fixed

- **Firmware update over the bus failed** (GUI). Three causes, each confirmed on the board:
  smpclient was imported after the DYNAMIXEL Reboot, which took about 0.8 s on Windows, longer
  than the 500 ms in which MCUboot waits for SMP; MCUboot answers the echo with rc 8 (echo not
  built in), which the GUI took for no answer; and MCUboot received on USART2 at 1 Mbps
  without the FIFO, so upload chunks were lost. The GUI now prepares the SMP client before
  the Reboot and takes any answer as MCUboot listening, and MCUboot has `fifo-enable`
  (`app/sysbuild/mcuboot.overlay`). Three full updates in a row: 85 KiB in 5 s each. A board
  with the old MCUboot needs the new merged HEX once through the ST-LINK.
- **Factory limits of the real motor:** Current Limit 557 (1498 mA, the run current) and
  Velocity Limit 1023 (234 rev/min, the largest X430 value) in place of the XM430 values
  1193 (3209 mA) and 200 (45.8 rev/min), at the first start and after Factory Reset
  (`x430_servo_set_factory_value()`). The servo node states the direct drive 1:1 of the
  17HS19-2004S1. The voltage limits stay those of the XM430
  ([DYNAMIXEL emulation](docs/dynamixel.md#factory-values-and-the-eeprom)).
- **GUI on Windows lost most answers** of the board. `SerialLink.read()` changed the port
  timeout on every read; on Windows pyserial then reconfigures the port, and the ST-LINK
  Virtual COM Port re-initialises its UART and loses the bytes in flight (12 of 100 answers
  received). The timeout is now fixed when the port opens (`tools/gui/tests/test_link.py`).

### Changed

- **Stepper-Lab follows `main`** in `west.yml` during development (it was pinned to a commit):
  its model change, one `sin` / `cos` per RK4 step, takes the fast loop of the simulated motor
  from 47.5 to 37 µs during a move. Pin it again before a release; `scripts/package_release.py`
  refuses a branch.
- **power_stage API:** the fast-loop callback takes `uint32_t trips` (`POWER_STAGE_TRIP_AUX`,
  `POWER_STAGE_TRIP_OVERRUN`) in place of `bool aux_trip`. Out-of-tree backends and callbacks
  must be updated ([Porting](docs/porting.md)).
- **Printf without floating point** (`CONFIG_CBPRINTF_FP_SUPPORT=n` in `app/prj.conf`): no log
  prints a float, and picolibc now links its integer printf. About 4.8 KiB less flash in every
  board build; the simulated motor fits next to MCUboot again (2.9 KiB to spare).
- **Dead time set to 1200 ns** (`dead-time-ns` in `app/boards/b_g431b_esc1.overlay`). The
  real-motor build now enables the power stage on Torque Enable. The value comes from an
  ngspice sweep of one leg with the gate circuit of schematic MB1419 rev C (L6387ED at 10 V,
  33 Ω gate resistors, 1 Ω + 10 nF snubber): shoot-through up to 600 ns, the first clean value
  at 900 ns, plus a margin of 1.3. It is not yet measured on the board: follow bring-up step 1
  ([Hardware and bring-up](docs/hardware.md#bring-up-checklist)) before connecting a motor.

## [0.1.0] - 2026-11-08

First public release: firmware for the B-G431B-ESC1 that answers as a DYNAMIXEL XM430-W350,
verified on the PC against a simulated motor. **The board build does not drive a real motor
yet**: the dead time of the power stage is not set, and until it is, the firmware keeps the
outputs off (see *Known limits*).

### Added

- DYNAMIXEL XM430-W350 emulation (model 1020) over Protocol 2.0 with the X430 Control Table
  Emulator: the complete Control Table, position, extended position and velocity mode,
  Sync/Bulk and Fast Sync/Bulk Read, Bus Watchdog, Reboot, Factory Reset, Clear Multi-Turn,
  EEPROM area in flash.
- Closed-loop microstepping current control with Stepper-C v1.2.0: current loop at 20 kHz in a
  zero-latency interrupt, motion profiles at 1 kHz, 16 microsteps per full step.
- Protection in the 20 kHz interrupt: overcurrent, bus over- and undervoltage and shared-leg
  faults switch the outputs off, reported in Hardware Error Status.
- Zephyr drivers: the `power_stage` driver class (STM32G4 and simulation backends), the motor
  behind Zephyr's `stepper` / `stepper_ctrl` APIs, the servo as a child node of its UART, the
  out-of-tree board `b_g431b_esc1`, devicetree bindings; layer rules checked in CI.
- MCUboot with an RSA-2048 signature check and serial recovery over the DYNAMIXEL bus.
- Simulated motor: the Stepper-Lab plant model with power stage values from ngspice, on the
  board (`sim.overlay`) and on `native_sim`.
- Tests: ztest suites, SIL and integration tests (DynamixelSDK, a ros2_control loop at
  100 Hz) and fixed-point motion tests on every CI run, in a published environment image.
- Bench GUI: motion with live plots and CSV recording, the Control Table, the SIL scenarios
  on the board with an HTML report, firmware update.
- Documentation website and PDF manual, with a download button for the PDF in the header of
  every page.
- Release pipeline: on a `vX.Y.Z` tag, every CI gate in the environment image, then the board
  images, the PDF manual and checksums as a GitHub Release.

### Release files

| File | Content |
| --- | --- |
| `doppelstep-<version>-b_g431b_esc1-merged.hex` | MCUboot and the signed application: the first flashing through the debug probe |
| `…-mcuboot.hex` | MCUboot alone |
| `…-app.signed.hex`, `…-app.signed.bin` | Signed application, real motor; the `.bin` is the file for an update over the bus (GUI) |
| `…-sim-motor.signed.hex`, `…-sim-motor.signed.bin` | Signed application with the simulated motor (`sim.overlay`), safe without a motor |
| `…-app.elf`, `…-sim-motor.elf` | Debug symbols |
| `doppelstep-<version>-manual.pdf` | User manual |
| `SHA256SUMS` | Checksums of the files above |

### Known limits

- **Dead time not set:** `dead-time-ns` in `app/boards/b_g431b_esc1.overlay` is 0 until the
  value of the board's motor-control reference project is entered; until then the real-motor
  build never switches the power stage on. Use the simulated-motor image on the board.
- **Development signing key:** the images are signed with the public development key of
  MCUboot. Any image signed with that key boots; for a product, sign with your own key
  ([Firmware update](docs/firmware-update.md)).
- The driver refactor is not yet checked on the board ([hardware checklist](docs/hardware-checklist.md)).
- Lost steps are not reported (the motor has no encoder).
- The temperature sensor is not converted: no overheating trip.
- The X430 emulator, Stepper-Lab and sfs-stepper-power-spice have no releases yet: `west.yml`
  pins them to the tested commits.

[Unreleased]: https://github.com/SystemFromScratch/sfs-doppelstep/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/SystemFromScratch/sfs-doppelstep/releases/tag/v0.1.0
