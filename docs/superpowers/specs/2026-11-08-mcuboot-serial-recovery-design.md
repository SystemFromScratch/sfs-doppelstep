# MCUboot serial recovery over USART2 — design

Date: 2026-11-08
Status: approved design, implementation pending

## Goal

Update the firmware of the B-G431B-ESC1 over USART2, the UART that carries the DYNAMIXEL bus,
with the bootloader Zephyr provides: MCUboot built by sysbuild, with its serial recovery
(SMP / MCUmgr protocol in the bootloader). Nothing custom on the protocol side.

## Decisions

| Topic | Decision |
|---|---|
| Mechanism | MCUboot serial recovery in the bootloader, single application slot. No MCUmgr in the application. |
| Entry into recovery | Wait window after every reset (`CONFIG_BOOT_SERIAL_WAIT_FOR_DFU`, 500 ms) and no valid image (`CONFIG_BOOT_SERIAL_NO_APPLICATION`). No retention / boot mode, no button. |
| Trigger from the bus | DYNAMIXEL Reboot (0x08) resets the MCU on the board, which opens the wait window. On native_sim Reboot stays emulated. |
| Bootloader baud rate | Fixed 1 Mbps, independent of the Baud Rate register of the application. |
| Signature | Sysbuild default: MCUboot development key (RSA-2048), `west sign` through sysbuild. Replaceable later with `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. |
| Scope | Mandatory for `b_g431b_esc1`: every board build uses sysbuild with MCUboot. native_sim and Twister unchanged. |
| Host tool | `smpmgr` (Python), documented. No update script: the device GUI, the next step, will drive updates. |

Why not MCUmgr in the application: its UART transport needs the UART exclusively, while USART2
is the DYNAMIXEL bus, and two slots plus the bootloader plus storage leave under about 45 KiB
per image in 128 KiB of flash.

## 1. Flash layout and build

STM32G431CB: 128 KiB of flash, 2 KiB pages.

```text
0x00000  boot_partition     MCUboot               size N
N        slot0_partition    application (signed)  up to 0x1f000
0x1f000  storage_partition  NVS, DYNAMIXEL EEPROM 4 KiB, unchanged
```

- N is the measured size of the MCUboot build, rounded up to a 2 KiB page. It is not guessed.
- The partitions live in `boards/st/b_g431b_esc1/b_g431b_esc1.dts`.
  `zephyr,code-partition = &slot0_partition` stays.
- `storage_partition` keeps its address, so the EEPROM data survives the change.
- `west.yml`: add `mcuboot` to the `name-allowlist`, plus any module the default signature type
  needs (to verify against MCUboot in Zephyr v4.5.0).
- `app/sysbuild.conf`: `SB_CONFIG_BOOTLOADER_MCUBOOT=y`, `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`.
- Board builds add `--sysbuild`. native_sim builds and Twister do not use sysbuild.

## 2. MCUboot configuration

`app/sysbuild/mcuboot.conf`:

- `CONFIG_MCUBOOT_SERIAL=y`, `CONFIG_BOOT_SERIAL_UART=y`
- `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU=y`, `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=500`
- `CONFIG_BOOT_SERIAL_NO_APPLICATION=y`: in single-application mode recovery writes slot 0
  directly, so after an interrupted upload the bootloader stays in recovery.
- UART console and logging off: USART2 carries SMP.

`app/sysbuild/mcuboot.overlay`:

- `&usart2 { current-speed = <1000000>; }`
- USART2 selected as the SMP UART through the `chosen` node MCUboot reads (to verify in the
  MCUboot version of Zephyr v4.5.0); console chosen nodes removed.

## 3. Application: Reboot resets the MCU on the board

- New Kconfig option in `drivers/x430/Kconfig`, `CONFIG_SFS_DXL_REBOOT_RESETS_MCU`,
  `default y if BOOTLOADER_MCUBOOT`. It is `n` on native_sim, so the SIL tests keep the
  emulated XM430 Reboot.
- `drivers/x430/x430_servo.c`, same pattern as `factory_baud_pending`: `X430_EVENT_REBOOT` sets a
  pending flag. `x430_servo_process()`, after the status packet was sent, waits ten character
  times at the current baud rate for the last bytes to leave and calls
  `sys_reboot(SYS_REBOOT_COLD)`. Ten, not two: with `fifo-enable` USART2 has an 8-byte TX FIFO,
  and `uart_poll_out()` returns as soon as the FIFO has room, so up to nine characters can still
  be in flight.
- Safety: `joint_bridge` already answers Reboot with a synchronous `APP_CMD_DISABLE`, so the
  outputs are off before the reset.
- Documented consequences on the board: after Reboot the position restarts (Stepper-C keeps it
  in RAM, there is no encoder), unlike an XM430; the device does not answer for the wait window
  plus the startup time.

## 4. Tools, CI, documentation

- `scripts/ci.sh` (esc1, esc1-sim), `.vscode/tasks.json`, `.vscode/launch.json`: `--sysbuild` and
  the sysbuild output paths (application domain and `mcuboot` domain; exact names to verify).
  The Windows OpenOCD flash task programs both images.
- `Dockerfile` / `requirements.txt`: `imgtool` dependencies (`mcuboot/scripts/requirements.txt`)
  and `smpmgr`.
- New page `docs/firmware-update.md` (flash layout, entry into recovery, the `smpmgr` upload at
  1 Mbps, the fixed bootloader baud rate whatever the Baud Rate register says), added to
  `mkdocs.yml`. Updates to `docs/architecture.md` (startup, flash) and `docs/development.md`
  (build and flash commands).

## Verification

- CI builds `esc1` and `esc1-sim` with sysbuild, pristine, without warnings; the build fails if
  the application does not fit slot 0.
- Layer checks, Twister suites and SIL tests unchanged and passing on native_sim.
- On hardware, by hand: DYNAMIXEL Reboot, `smpmgr` upload within the window, the new version
  starts; an interrupted upload leaves the bootloader in recovery and a second upload succeeds;
  the EEPROM values survive.

## Out of scope

- MCUmgr in the application, two-slot swap, image confirmation and rollback.
- A production signing key.
- An update script: the device GUI will drive updates.
