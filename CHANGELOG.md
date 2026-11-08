# Changelog

All notable changes to Doppelstep are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The version of a release is its
`vX.Y.Z` tag; `app/VERSION` holds the same number, and the release pipeline checks both
([Development environment](docs/development.md#releasing)).

## [Unreleased]

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
