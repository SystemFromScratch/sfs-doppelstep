# Changelog

All notable changes to Doppelstep are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The version of a release is its
`vX.Y.Z` tag; `app/VERSION` holds the same number, and the release pipeline checks both
([Development environment](docs/development.md#releasing)).

## [Unreleased]

## [0.3.0] - 2026-11-14

### Added

- **Firmware version from git**: `cmake/doppelstep_version.cmake` runs `git describe` at every
  build (app/VERSION without git or tag, with a warning when the two differ) and generates
  `doppelstep_version.h` with `configure_file()`. The boot log shows it (`Doppelstep
  0.2.0+3.gabc1234-dirty`), Model Information (2) reports it (byte 0 patch, 1 minor, 2 major,
  3 commits after the tag, bit 31 uncommitted changes), and the GUI decodes it after Connect,
  in the reports and after a firmware update. Firmware Version (6) stays 52 (XM430).
  SIL test `test_model_information_is_the_build_version`.

- **Measurements with MCUViewer** ([Measurements with MCUViewer](docs/measurements.md)):
  `CONFIG_SSTEP_MOTOR_MCUVIEWER`, off by default, publishes the measured phase currents,
  the commanded phase voltages, their averages and the peak-to-peak current of the fast
  loop in the global variable `mcuv`, and records them at up to 20 kHz into `mcuv_buf`
  on request. `app/mcuviewer.conf` makes the bench build. `tools/mcuviewer/mcuv.py`
  starts a capture over the SWD probe and evaluates the current ripple at rest (hardware
  checklist, check 15) and the effective dead time, without a current probe, with a table
  of the loss per current range. `tools/mcuviewer/doppelstep.mcuv` is a ready MCUViewer
  project. CI builds the bench build and checks that the release build does not contain
  the option.
- **Measured on the board** (B-G431B-ESC1, 17HS19-2004S1, 24 V and 12 V;
  [test record](docs/test-record.md)): the current ripple at rest is below the resolution
  of the ADC (about 31 mA rms noise, the same with the torque off; no toggle at 125 Hz).
  The effective dead time is 1.05 µs at 24 V and 1.4 µs at 12 V (set 1.2 µs). Open: the
  fit gives 2.75 Ω for the phase circuit at both voltages instead of 1.6 Ω.

### Changed

- **DYNAMIXEL-X430 emulator v1.0.0** (was a pinned commit of `main`): smaller RAM, flash and
  stack (the application needs 20 828 B of RAM, 1.4 KiB less, with the larger packet buffer).
  The driver `drivers/x430/` follows its API: functions return `X430_OK` or `X430_ERR_*` and
  give values through pointers; the persistent blocks are read and written in parts
  (`nvm_read` / `nvm_write`), which `x430_nvm.c` collects into one NVS item per block.
- **Factory values from the devicetree, shown in `doppelstep_config.h`**:
  `scripts/gen_doppelstep_config.py` generates the header at CMake configure time
  (`build/app/doppelstep/`) with every factory value of the EEPROM area, its source and
  range, and the driver data (bus voltage limits and trips, currents, velocity, gear). The
  firmware writes the values from the devicetree: Temperature Limit 80 °C (new property
  `temperature-limit-celsius` of the power stage), Max / Min Voltage Limit 28 / 10 V
  (`vbus-max-mv` / `vbus-min-mv`, was 16 / 9.5 V), Current Limit 743 = 2000 mA
  (`rated-current-milliamp`, was 557 = 1500 mA), Velocity Limit 1023. DYNAMIXEL Wizard no
  longer warns about the 24 V supply. The voltage limit registers accept 9.5–30 V
  (`app/x430_overrides.yaml`, `CONFIG_X430_MODEL_OVERRIDES` of the DYNAMIXEL-X430 emulator
  v1.0.0). A board with an EEPROM area stored by 0.3.0 keeps it until a Factory Reset.
- **Current Limit raises the run current up to the rated current** (2000 mA, was capped at
  `run-current-milliamp`): the run current is still 1500 mA at startup and after a Factory
  Reset (`x430_servo_factory_write()`). `sstep_motor_set_current()` limits the run current
  to `rated-current-milliamp`. `CONFIG_SFS_DXL_FACTORY_VALUES_MAX` defaults to 8. SIL test
  `test_current_limit_raises_the_run_current_up_to_the_rated_current`.
- **Factory baud rate 57600 bps, as a real XM430-W350** (was 1 Mbps): `factory-baud-code`
  defaults to 1, USART2 starts at 57600 bps, and DYNAMIXEL Wizard finds the board with its
  default scan. The GUI connects at 57600 bps by default (`--baud`, `FACTORY_BAUD` in
  `registers.py`) and its test scenarios expect it. For OpenMANIPULATOR-X write Baud Rate 3
  (1 Mbps) once, or set `factory-baud-code = <3>`. MCUboot serial recovery stays at a fixed
  1 Mbps. A board with a stored EEPROM block keeps its Baud Rate until a Factory Reset.
- **STATUS LED follows the LED register (65)**, as the LED of an XM430: 1 on, 0 off, so
  the host (DYNAMIXEL Wizard, the GUI) switches it. An alarm (Hardware Error Status not 0,
  motor fault, startup error) still makes it blink fast. The slow blink with the outputs
  off and the steady light with the torque on are gone: Torque Enable (64) shows that state.

### Fixed

- **A write of Baud Rate (8) got no answer**: the bus switched to the new rate while the
  last bytes of the status packet were still in the 8-byte TX FIFO of USART2, and
  `uart_configure()` cut them, so DYNAMIXEL Wizard saw a time-out and lost the device. The
  driver now waits ten character times at the old rate before it reconfigures the UART
  (`wait_tx_drained()`, shared with the Reboot reset).
- **Present PWM (124) stayed 0**: it is now the amplitude of the commanded phase duty
  vector (from the compare values of the fast loop), averaged over 8 ms, 885 = 100 %, with
  the sign of Present Velocity. New field `pwm_permille` of `struct sstep_motor_status`,
  `joint_pwm_to_dxl()`, ztest `test_pwm` and SIL test `test_present_pwm_follows_the_drive`.
- **DYNAMIXEL Wizard 2.0 showed TIME OUT** after a successful scan: its Dashboard reads
  the whole Control Table (addresses 0–661) in one READ of 662 bytes, and the DXL2-C
  packet buffer of 256 bytes allowed 245 bytes at most (Data Length Error).
  `CONFIG_X430_DXL2_MAX_PACKET_SIZE=768` answers READs of up to 757 bytes; the comm task
  stack grows to 3584 bytes for the two packet buffers on it. SIL test
  `test_whole_control_table_in_one_read`
  ([DYNAMIXEL emulation](docs/dynamixel.md#dynamixel-wizard-20)).

### Upgrading from 0.2.0

- **The stored EEPROM area of 0.2.0 is not taken over.** The emulator v1.0.0 stores its
  blocks with a header and a CRC; the firmware keeps them under new NVS ids and deletes the
  old items at the first start. The board then starts with the factory values of 0.3.0 once:
  ID 1, 57600 bps, the new limits. Write ID, Baud Rate and other settings again if they were
  changed.
- **The factory baud rate is now 57600 bps** (was 1 Mbps). Hosts that start from the factory
  values (ROS 2 descriptions, scripts) use 57600 bps, or write Baud Rate 3 once for 1 Mbps.
  MCUboot serial recovery stays at 1 Mbps.
- The STATUS LED follows the LED register: after startup it is off, not blinking.

### Known limits

- PWM Limit (36) and the gains of the Velocity PI / Position PID controllers (76–90) are
  stored but not applied: current drive without an encoder. A duty limit in Stepper-C is
  planned; the gains wait for an encoder.
- Lost steps are not reported (the motor has no encoder).
- The dead time of 1200 ns is measured as 1.05 µs effective at 24 V
  ([test record](docs/test-record.md)); the gate voltages are not yet checked with an
  oscilloscope.

## [0.2.0] - 2026-11-13

The real-motor build drives a motor. Checked on the B-G431B-ESC1 with a 17HS19-2004S1 at
12 V and 24 V: all 20 GUI test scenarios in three runs, the hardware comparators, two minutes
of moves at 1.5 A, and the self-test against no supply, an open coil, no motor, a coil on the
wrong legs and swapped coils. The bridge and the motor are protected in layers, from
comparators into the timer break input to a self-test before the first Torque Enable. Torque
Enable is answered at once, as on an XM430. Motor control is Stepper-C 1.4.0. The tests on the
board and their results are in the [test record](docs/test-record.md).

### Added

- **Protection of the bridge and the motor** ([Protection](docs/protection.md)):
  - Hardware trips on the B-G431B-ESC1: COMP1/2/4 watch the OPAMP inputs against DAC3
    (10 A, `hw-trip-current-milliamp`) and COMP3 the bus divider against DAC1 (30 V,
    `hw-trip-vbus-mv`); they drive the TIM1 break inputs, which switch the gates off within
    0.5 µs without software. Reported as `POWER_STAGE_TRIP_HW_OC` / `_HW_OV`, latched until a
    reset.
  - The leg trip checks U, V and W in the interrupt (was W only), so the motor current is
    also checked in voltage drive.
  - Current offset window (`current-offset-min-counts`, `-max-counts`, 2300..2800): a broken
    amplifier, shunt or bias network refuses the outputs; the bus keeps running and reports
    Electrical Shock.
  - Torque Enable with the bus outside `vbus-min-mv`..`vbus-max-mv` answers Result Fail with
    Input Voltage Error before anything is energised, and latches nothing.
  - Self-test of the bridge and the motor at the first Torque Enable
    (`CONFIG_SSTEP_MOTOR_SELFTEST`, `selftest-current-milliamp` 600 mA): `sstep_selftest()` of
    Stepper-C gives each coil the test current at a slowly rising voltage, and the motor driver
    adds up the three leg currents meanwhile (a broken sensor: above a quarter of the test
    current for 2 ms in a row; one disturbed sample does not count). An open coil, a short, a wrong current sign, a coil
    on the wrong legs, a broken current sensor or a leaking switch turns the torque off again
    with Electrical Shock (`SSTEP_MOTOR_FAULT_SELFTEST`) and a log message that names the
    cause. Torque Enable is answered at once, as on an XM430: `stepper_enable()` starts the
    test, the slow loop ends it and runs a Goal Position written meanwhile (DynamixelSDK waits
    only about 34 ms for the status packet; 2 ms on the board at 24 V). Off
    in the simulated-motor build on the board (flash, `CONFIG_SSTEP_SELFTEST` =
    `SSTEP_CFG_SELFTEST`), on in `native_sim`.
  - `i_aux` (current of the shared leg) in `struct power_stage_sample`.
- **Sampling window and current slew** (Stepper-C 1.3): `sample-window-ns` of
  the power stage node (2400 ns on the B-G431B-ESC1) keeps every leg on its low-side switch
  around the samples; `current-slew-ma-per-ms` of the motor node (30) lets the current rise
  after Torque Enable. Found at the first run with the motor: Torque Enable at 1 A and 1.5 A
  from 12 V tripped the undervoltage protection, because a duty cycle near 100 % corrupted the
  bus voltage sample ([Hardware](docs/hardware.md#sampling-window)).
  `power_stage_info.sample_window_ticks` in the class API.

- **Power stage temperature:** the NTC of the B-G431B-ESC1 (TDK NTCG103JF103FT1 on PB14) is
  converted in every PWM period and shown in Present Temperature (146) and in the GUI. Above
  Temperature Limit (31, 80 °C) Overheating Error switches the torque off until a Reboot, as on
  an XM430; an open or shorted NTC trips too. `power_stage_get_temperature()` in the class API,
  NTC properties of the power stage node.
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
- **GUI Motion tab:** Goal Position and Goal Velocity are enabled only in the Operating Mode
  in which they act (the device stores the other one and does nothing with it); Profile
  Velocity and Acceleration show their physical value (2 = 0.458 rev/min); the Goal Velocity
  frame fits in the window again.
- **GUI torque button:** one button in the Motion tab shows the torque state from the live
  registers and toggles it: green *TORQUE ON*, red *TORQUE OFF*. It replaces Torque ON and STOP.

### Fixed

- **The Stepper-C options of Kconfig reached the library only by chance:**
  `modules/stepper-c/CMakeLists.txt` built them in `SSTEP_COMPILE_DEFINITIONS`, but
  `sstep.cmake` applies the variable `COMPILE_DEFINITIONS`, so the library always had its
  defaults. These equalled the Kconfig defaults until `CONFIG_SSTEP_SELFTEST` came. The glue
  now sets `COMPILE_DEFINITIONS`, which `sstep.cmake` puts on the library with its public
  scope, so the driver that holds the instance sees the same `SSTEP_STATE_BYTES`.
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
- **GUI: an answer that arrived after the poller's deadline was thrown away.** `Bus.receive()`
  gave up before reading the port once more, so a poller thread held past its 0.1 s by the GUI
  thread reported "no answer" although the answer was waiting (`test_answer_waiting_after_the_deadline_is_not_lost`).
- **GUI on Windows lost most answers** of the board. `SerialLink.read()` changed the port
  timeout on every read; on Windows pyserial then reconfigures the port, and the ST-LINK
  Virtual COM Port re-initialises its UART and loses the bytes in flight (12 of 100 answers
  received). The timeout is now fixed when the port opens (`tools/gui/tests/test_link.py`).

### Changed

- Present Current is averaged over 8 ms and reads 0 with the torque off: on the board a single
  sample moved by an ADC step (29 mA), and the sensor offset drift showed 30 to 60 mA with no
  current flowing.

- Stepper-C v1.4.0 (was v1.2.0): the self-test, the bus voltage check of the enable
  (`SSTEP_ERR_VBUS` → `-ERANGE`), the trip of the shared leg, the sampling window and the current
  slew come from the library. 1.4.0 waits for the rotor to come to rest in every check of
  the self-test: 1.3.0 reported a coil on the wrong legs once on the board, with the rotor a
  full step away at 24 V. `CONFIG_SSTEP_STALL=n` (`SSTEP_CFG_STALL`, new in 1.4.0) removes the
  unused stall detection: the real-motor image is 77 420 B (was 80 268 B), the
  simulated-motor image 84 100 B with 3 612 B left.
- Torque Enable below `vbus-min-mv` is refused at once; before, it was accepted and the
  undervoltage trip latched a fault.

- **Stepper-Lab** pinned to `9fc2eef` (was `main` during development): its model change, one
  `sin` / `cos` per RK4 step, takes the fast loop of the simulated motor from 47.5 to 37 µs
  during a move.
- **power_stage API:** the fast-loop callback takes `uint32_t trips` (`POWER_STAGE_TRIP_AUX`,
  `POWER_STAGE_TRIP_OVERRUN`) in place of `bool aux_trip`. Out-of-tree backends and callbacks
  must be updated ([Porting](docs/porting.md)).
- **Printf without floating point** (`CONFIG_CBPRINTF_FP_SUPPORT=n` in `app/prj.conf`): no log
  prints a float, and picolibc now links its integer printf. About 4.8 KiB less flash in every
  board build; the simulated motor fits next to MCUboot again.
- **Dead time set to 1200 ns** (`dead-time-ns` in `app/boards/b_g431b_esc1.overlay`). The
  real-motor build now enables the power stage on Torque Enable. The value comes from an
  ngspice sweep of one leg with the gate circuit of schematic MB1419 rev C (L6387ED at 10 V,
  33 Ω gate resistors, 1 Ω + 10 nF snubber): shoot-through up to 600 ns, the first clean value
  at 900 ns, plus a margin of 1.3. It is not yet measured on the board: follow bring-up step 1
  ([Hardware and bring-up](docs/hardware.md#bring-up-checklist)) before connecting a motor.

### Known limits

- **Dead time not measured on the board:** 1200 ns comes from the ngspice sweep. The motor ran
  at 12 V and 1.5 A without a fault or a hot switch, but confirm the value with an
  oscilloscope before higher voltages ([bring-up step 1](docs/hardware.md#bring-up-checklist)).
- **Hardware checklist not complete:** the current ripple and the vibration at rest (checks
  15, 16), the NTC against a thermometer and the fast-loop time of the real-motor build are
  open
  ([hardware checklist](docs/hardware-checklist.md)).
- Once, in the first run of the GUI scenarios with the motor, Torque Enable was refused in the
  Bus Watchdog scenario; it did not happen in the 13 runs after it, and the RTT log lost the
  reason.
- Lost steps are not reported (the motor has no encoder).
- The simulated-motor image with MCUboot leaves out the self-test, the hardware comparators
  and the offset window, for flash ([Simulated motor](docs/simulation.md#flash-budget)).
- **Development signing key:** the images are signed with the public development key of
  MCUboot; for a product, sign with your own key ([Firmware update](docs/firmware-update.md)).
- The X430 emulator, Stepper-Lab and sfs-stepper-power-spice have no releases yet: `west.yml`
  pins them to the tested commits.

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

[Unreleased]: https://github.com/SystemFromScratch/sfs-doppelstep/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/SystemFromScratch/sfs-doppelstep/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/SystemFromScratch/sfs-doppelstep/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/SystemFromScratch/sfs-doppelstep/releases/tag/v0.1.0
