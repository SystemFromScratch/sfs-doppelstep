# MCUboot Serial Recovery over USART2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Firmware update of the B-G431B-ESC1 over USART2 through MCUboot serial recovery, built by sysbuild.

**Architecture:** Sysbuild builds MCUboot (single application slot, serial recovery on USART2 at 1 Mbps, 500 ms wait window, recovery when no valid image) and the signed application. The application is unchanged except that DYNAMIXEL Reboot (0x08) resets the MCU on the board (new Kconfig option of the X430 driver), which opens the wait window. native_sim and Twister keep building without sysbuild.

**Tech Stack:** Zephyr v4.5.0, sysbuild, MCUboot (from the Zephyr v4.5.0 manifest), imgtool, smpmgr (host), bash CI in Docker.

**Spec:** `docs/superpowers/specs/2026-11-08-mcuboot-serial-recovery-design.md`

## Global Constraints

- Zephyr `v4.5.0`; MCUboot is the revision the Zephyr v4.5.0 manifest pins (imported through `name-allowlist`, never pinned separately).
- Flash: 128 KiB, 2 KiB pages. `boot_partition` at `0x0`, size N = measured MCUboot size rounded up to 2 KiB; `slot0_partition` from N to `0x1f000`; `storage_partition` stays `0x0001f000`, `DT_SIZE_K(4)`.
- `SB_CONFIG_BOOTLOADER_MCUBOOT=y`, `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`, default signature type and development key (no key file in the repository).
- MCUboot: `CONFIG_MCUBOOT_SERIAL=y`, `CONFIG_BOOT_SERIAL_UART=y`, `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU=y`, `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=500`, `CONFIG_BOOT_SERIAL_NO_APPLICATION=y`, no UART console, no logging; USART2 `current-speed = <1000000>`.
- New option `CONFIG_SFS_DXL_REBOOT_RESETS_MCU`, `default y if BOOTLOADER_MCUBOOT`; off on native_sim.
- Every board build uses `--sysbuild`; native_sim builds and Twister do not.
- Repository content in English. The user commits on `main`: a "commit" step means handing the user the file list and a proposed message, with no AI mention. Never run `git commit`.
- "[verify]" in a step means: read the named source in the west workspace (`<topdir>/bootloader/mcuboot`, `<topdir>/zephyr`) and use what it says; when it contradicts this plan, stop and report.
- Builds and tests run in the Docker image: `docker compose run --rm zephyr bash -lc '<command>'` from the repository root (Windows host), or directly in the Dev Container.

## Review Focus

1. Loading the unsigned `zephyr.elf` with a debugger: MCUboot rejects it and stays in recovery, so the motor firmware never starts. Every debug configuration must load the MCUboot image and the signed application (Task 3).
2. `-DEXTRA_DTC_OVERLAY_FILE=sim.overlay` (and `-DSTEPPER_LAB_SOURCE_DIR`) leaking into the MCUboot image: the bootloader would get the simulated power stage node. The CI check asserts the MCUboot devicetree has no `sfs,sim-power-stage` (Task 1).
3. The host set the Baud Rate register to something other than 1 Mbps: the application answers at that rate, the bootloader still listens at 1 Mbps. The CI check pins the bootloader speed (Task 1); the documentation says it in the troubleshooting table (Task 4).
4. Reboot at a low baud rate (9600, 57600): the reset must not cut the status packet. The hardware check runs Reboot at 57600 (Task 4).
5. Upload interrupted halfway: slot 0 is invalid and the board must stay in recovery and accept a second upload; the EEPROM values in `storage_partition` must survive every update (Task 4).

---

### Task 1: MCUboot, flash layout and sysbuild in CI

**Files:**
- Modify: `west.yml` (`name-allowlist`)
- Modify: `boards/st/b_g431b_esc1/b_g431b_esc1.dts` (`&flash0` partitions)
- Create: `app/sysbuild.conf`
- Create: `app/sysbuild/mcuboot.conf`
- Create: `app/sysbuild/mcuboot.overlay`
- Modify: `scripts/ci.sh` (board builds, new checks)
- Modify: `Dockerfile` (imgtool dependencies)

**Interfaces:**
- Produces: sysbuild build directory layout `<dir>/<app-domain>/zephyr/zephyr.elf`, `<dir>/<app-domain>/zephyr/zephyr.signed.hex|bin`, `<dir>/mcuboot/zephyr/zephyr.hex|elf`. Record the actual application domain name (expected `app`, the source directory name, [verify] in `zephyr/share/sysbuild/CMakeLists.txt`) in the hand-over; Tasks 2–4 use it as `<app-domain>`.
- Produces: `build_checks()` in `scripts/ci.sh`, the place where Task 2 adds its check.

- [ ] **Step 1: Write the failing CI checks in `scripts/ci.sh`**

Change the two board builds to `west build -p always --sysbuild ...` and add, after the three builds and before the `failed` test, a function `build_checks` that sets `failed=1` with an `ERROR:` line when any of these fails:

```bash
# MCUboot image exists and the application is signed
test -f build-ci/esc1/mcuboot/zephyr/zephyr.hex
test -f build-ci/esc1/app/zephyr/zephyr.signed.bin
# Bootloader: SMP on USART2 at 1 Mbps, wait window, no console
grep -q 'current-speed = < 0xf4240 >' build-ci/esc1/mcuboot/zephyr/zephyr.dts
grep -q '^CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=500$' build-ci/esc1/mcuboot/zephyr/.config
grep -q '^CONFIG_BOOT_SERIAL_NO_APPLICATION=y$' build-ci/esc1/mcuboot/zephyr/.config
grep -q '^# CONFIG_UART_CONSOLE is not set$' build-ci/esc1/mcuboot/zephyr/.config
# Storage partition unchanged
grep -q 'reg = < 0x1f000 0x1000 >' build-ci/esc1/app/zephyr/zephyr.dts
# sim.overlay reaches only the application
grep -q 'sfs,sim-power-stage' build-ci/esc1-sim/app/zephyr/zephyr.dts
! grep -q 'sfs,sim-power-stage' build-ci/esc1-sim/mcuboot/zephyr/zephyr.dts
```

Paths use the domain name `app`; change them if Step 4 finds another. Also print the MCUboot `FLASH:` line (the build log already shows both images' memory reports through the existing `grep -E 'FLASH:|RAM:'`).

- [ ] **Step 2: Run CI to verify it fails**

Run: `docker compose run --rm zephyr bash scripts/ci.sh`
Expected: FAIL. sysbuild builds the application only (no MCUboot enabled yet), the `test -f .../mcuboot/...` check prints `ERROR:`.

- [ ] **Step 3: Add MCUboot to the workspace**

- `west.yml`: add `mcuboot` to `name-allowlist`, with a comment line `# bootloader (docs/firmware-update.md)`.
- Run `west update` in the container (`bash scripts/west-init.sh`).
- [verify] which modules the MCUboot build with the default signature type needs (build errors name missing modules: typically `mbedtls`, possibly `zcbor`). Add each to `name-allowlist` with a comment saying what needs it. Add nothing the build does not ask for.
- `Dockerfile`: install the packages of `<topdir>/bootloader/mcuboot/scripts/requirements.txt` as pinned `ARG <NAME>_VERSION` lines in the existing pip `RUN`, under a comment `# imgtool (MCUboot image signing by sysbuild): bootloader/mcuboot/scripts/requirements.txt`. Pin each to the newest release that satisfies that file.

- [ ] **Step 4: Enable MCUboot**

- `app/sysbuild.conf`: `SB_CONFIG_BOOTLOADER_MCUBOOT=y`, `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`, with the SPDX header and a comment that the default development key is used (spec, "Signature").
- `app/sysbuild/mcuboot.conf`: the MCUboot options of the Global Constraints, plus `CONFIG_CONSOLE=n`, `CONFIG_UART_CONSOLE=n`, `CONFIG_LOG=n`. [verify] in `bootloader/mcuboot/boot/zephyr/Kconfig.serial_recovery` that the option names exist in this version and that `sysbuild/<image>.conf` is merged into (not replacing) MCUboot's `prj.conf` (`zephyr/share/sysbuild/` image configuration handling).
- `app/sysbuild/mcuboot.overlay`: `&usart2 { current-speed = <1000000>; };`, delete `zephyr,console` and `zephyr,shell-uart` from `chosen`, and set the chosen node MCUboot's serial adapter reads ([verify] in `bootloader/mcuboot/boot/zephyr/serial_adapter.c`, expected `zephyr,uart-mcumgr = &usart2;`).
- Board DTS: replace the `&flash0` partitions with `boot_partition: partition@0` (`label = "mcuboot"`, provisional `reg = <0x00000000 DT_SIZE_K(48)>`), `slot0_partition: partition@c000` (`label = "image-0"`, `reg = <0x0000c000 DT_SIZE_K(76)>`), `storage_partition` unchanged. Keep the `compatible` lines the existing partitions use, and update the comment block above with the layout table of the spec.

- [ ] **Step 5: Measure MCUboot and set the final partition sizes**

Run: `docker compose run --rm zephyr bash -lc 'west build -p always --sysbuild -b b_g431b_esc1 -s app -d build-ci/esc1'`
Read the `FLASH:` line of the MCUboot image. N = that size rounded up to a multiple of 2 KiB. Set `boot_partition` to `DT_SIZE_K(N/1024)`, `slot0_partition` to `partition@<N hex>`, `reg = <N 0x1f000-N>`.
Stop and report to the user (do not change the signature type) if N > 0x1f000 − application FLASH size rounded up to 2 KiB, i.e. the application does not fit.

- [ ] **Step 6: Run CI to verify it passes**

Run: `docker compose run --rm zephyr bash scripts/ci.sh`
Expected: `layers: ok`, all builds without warnings, no `ERROR:` from `build_checks`, Twister and SIL suites pass. If the `sim-power-stage` check fails, pass the overlay as `-Dapp_EXTRA_DTC_OVERLAY_FILE=sim.overlay` (and `-Dapp_STEPPER_LAB_SOURCE_DIR`) in the esc1-sim build and re-run.

- [ ] **Step 7: Hand over for commit**

Files: `west.yml`, `boards/st/b_g431b_esc1/b_g431b_esc1.dts`, `app/sysbuild.conf`, `app/sysbuild/mcuboot.conf`, `app/sysbuild/mcuboot.overlay`, `scripts/ci.sh`, `Dockerfile`. Message: `Build MCUboot with serial recovery on USART2 through sysbuild`. Report N, both FLASH lines and the application domain name.

---

### Task 2: DYNAMIXEL Reboot resets the MCU on the board

**Files:**
- Modify: `drivers/x430/Kconfig`
- Modify: `drivers/x430/x430_servo.c` (data struct, `X430_EVENT_REBOOT` case, end of `x430_servo_process()`)
- Modify: `include/doppelstep/drivers/x430_servo.h` (doc comment of `x430_servo_process()`)
- Modify: `scripts/ci.sh` (`build_checks`)

**Interfaces:**
- Consumes: `build_checks()` and the build directories of Task 1.
- Produces: `CONFIG_SFS_DXL_REBOOT_RESETS_MCU` (bool).

- [ ] **Step 1: Write the failing CI checks**

In `build_checks`:

```bash
grep -q '^CONFIG_SFS_DXL_REBOOT_RESETS_MCU=y$' build-ci/esc1/app/zephyr/.config
grep -q '^# CONFIG_SFS_DXL_REBOOT_RESETS_MCU is not set$' build-tests/zephyr/.config
```

- [ ] **Step 2: Run CI to verify it fails**

Run: `docker compose run --rm zephyr bash scripts/ci.sh`
Expected: FAIL with `ERROR:` on the first check (the symbol does not exist).

- [ ] **Step 3: Add the Kconfig option**

In `drivers/x430/Kconfig`, inside `if X430_SERVO`:

```kconfig
config SFS_DXL_REBOOT_RESETS_MCU
	bool "Reboot (0x08) resets the MCU"
	default y if BOOTLOADER_MCUBOOT
	select REBOOT
	help
	  After the status packet, the driver resets the MCU, so MCUboot opens
	  its serial recovery window (docs/firmware-update.md). The position
	  restarts, unlike on an XM430. Off: Reboot stays emulated (native_sim
	  and the SIL tests).
```

- [ ] **Step 4: Implement the reset in `x430_servo.c`**

- Add `bool reset_pending;` to `struct x430_servo_data`, next to `factory_baud_pending`, with the comment `/* Reboot: reset the MCU after the answer */`.
- In the `X430_EVENT_REBOOT` case, under `IS_ENABLED(CONFIG_SFS_DXL_REBOOT_RESETS_MCU)`, set `data->reset_pending = true` (keep the baud update). `X430_EVENT_BACKUP_RESTORED` must not set it: split the shared case.
- At the end of `x430_servo_process()`, when `reset_pending`: `LOG_INF("Reboot: MCU reset")`, `k_busy_wait()` for ten character times at `baud_bps[data->baud]` (10 bits per character: `100000000U / baud + 1U` µs), then `sys_reboot(SYS_REBOOT_COLD)` (`<zephyr/sys/reboot.h>`). The motor outputs are already off: `joint_bridge` sends `APP_CMD_DISABLE` synchronously in the event callback.
- Header: one sentence in the doc comment of `x430_servo_process()` that with `CONFIG_SFS_DXL_REBOOT_RESETS_MCU` it does not return after a Reboot.

- [ ] **Step 5: Run CI to verify it passes**

Run: `docker compose run --rm zephyr bash scripts/ci.sh`
Expected: no `ERROR:`, `layers: ok`; `test_reboot_disables_the_torque_and_keeps_the_position` and `tests/sil/test_protection.py` pass unchanged (Reboot stays emulated on native_sim).

- [ ] **Step 6: Hand over for commit**

Files: `drivers/x430/Kconfig`, `drivers/x430/x430_servo.c`, `include/doppelstep/drivers/x430_servo.h`, `scripts/ci.sh`. Message: `Reset the MCU on DYNAMIXEL Reboot when MCUboot is the bootloader`.

---

### Task 3: VS Code tasks, flashing and debugging with sysbuild

**Files:**
- Modify: `.vscode/workspace-menu.sh` (`build_firmware`, `require_build`)
- Modify: `.vscode/host-menu.ps1` (`$Elf`, `Invoke-HostFlash`)
- Modify: `.vscode/tasks.json` (`detail` strings of the board builds)
- Modify: `.vscode/launch.json` (all six configurations)

**Interfaces:**
- Consumes: `<app-domain>` and the image paths of Task 1.

- [ ] **Step 1: Build and flash scripts**

- `workspace-menu.sh`: `build_firmware` passes `--sysbuild`; `require_build` tests `${BUILD_DIR}/<app-domain>/zephyr/zephyr.elf`. `west flash -d ... -r openocd|jlink` stays (sysbuild flashes both images).
- `host-menu.ps1`: `$Elf` points to `build\<app-domain>\zephyr\zephyr.elf` (existence check); `Invoke-HostFlash` programs `build/mcuboot/zephyr/zephyr.hex` and then `build/<app-domain>/zephyr/zephyr.signed.hex`: `-c "program {<mcuboot>} verify" -c "program {<app>} verify reset exit"`.
- `tasks.json`: `detail` of the three board builds shows `west build --sysbuild -b b_g431b_esc1 ...`.

- [ ] **Step 2: Debug configurations**

Every configuration: `executable` = `${workspaceFolder}/build/<app-domain>/zephyr/zephyr.elf` (application symbols).
- The two `launch` configurations with `servertype` `openocd` / `jlink`: add `"loadFiles": ["${workspaceFolder}/build/mcuboot/zephyr/zephyr.hex", "${workspaceFolder}/build/<app-domain>/zephyr/zephyr.signed.hex"]`.
- The two `external` launch configurations: in `overrideLaunchCommands` replace `-target-download` with `load <mcuboot hex>` followed by `load <signed app hex>` (absolute paths through `${workspaceFolder}`).
- The two `attach` configurations: only the `executable` change.

- [ ] **Step 3: Verify on the board**

Run, in order, on the Windows host: task "Zephyr: Pristine Build", task "Zephyr: Flash (OpenOCD, ST-LINK)", then the debug configuration "Debug: OpenOCD on host".
Expected: flash verifies both images; the debugger stops at `main` of the application (so MCUboot accepted the signed image); RTT shows the line `DYNAMIXEL XM430-W350: ID 1, ...`. Repeat with one J-Link configuration if a J-Link is available, and say so in the hand-over if not.

- [ ] **Step 4: Hand over for commit**

Files: `.vscode/workspace-menu.sh`, `.vscode/host-menu.ps1`, `.vscode/tasks.json`, `.vscode/launch.json`. Message: `Flash and debug the MCUboot and the signed application images`.

---

### Task 4: Documentation, smpmgr and hardware verification

**Files:**
- Create: `docs/firmware-update.md`
- Modify: `mkdocs.yml` (nav entry after "Development environment")
- Modify: `docs/architecture.md` (Startup: MCUboot first; Configuration table: `app/sysbuild/`)
- Modify: `docs/development.md`, `docs/hardware-checklist.md`, `docs/simulation.md:102`, `docs/index.md:14`, `README.md` (build commands with `--sysbuild`, artifact paths, the size line at `README.md:333-334`)
- Modify: `requirements.txt` (`smpmgr`)

**Interfaces:**
- Consumes: N, the FLASH sizes and `<app-domain>` from Task 1; `CONFIG_SFS_DXL_REBOOT_RESETS_MCU` from Task 2.

- [ ] **Step 1: smpmgr**

`requirements.txt`: a section `# Firmware update over USART2 (docs/firmware-update.md)` with `smpmgr==<newest release>`. [verify] the exact upload command for MCUboot serial recovery in single-application mode in the smpmgr README of that version (image upload to slot 0, then reset; `upgrade` may send image-state commands that serial recovery does not support).

- [ ] **Step 2: Write `docs/firmware-update.md`**

Sections: flash layout (table with the measured N); how the board enters recovery (500 ms after every reset, DYNAMIXEL Reboot, invalid image); uploading with smpmgr at 1 Mbps (command from Step 1, the file `build/<app-domain>/zephyr/zephyr.signed.bin`); what Reboot means on the board (position restarts, no answer for the window plus startup, native_sim keeps the emulated Reboot); signature (development key, `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` for an own key); troubleshooting table: Baud Rate register ≠ 1 Mbps (the bootloader always listens at 1 Mbps), debugger loaded `zephyr.elf` (board stays in recovery: flash both images), interrupted upload (board stays in recovery: upload again). Style of the existing pages: short sentences, tables, no marketing.

- [ ] **Step 3: Update the other pages and run the documentation build**

Run: `docker compose run --rm zephyr bash -lc 'pip install -r requirements.txt >/dev/null && mkdocs build'` (or the "Docs" task of the workspace menu)
Expected: build passes under `strict: true`, no warning about `firmware-update.md` or a broken link.

- [ ] **Step 4: Hardware verification (by hand, with the user)**

On the board, flashed by Task 3. Throwaway helper only, in the scratchpad, not committed: a Python snippet that opens the COM port with pyserial at the current baud rate, sends `tools/sim/dxl2.packet(1, 0x08)`, reads the status, then switches to 1 Mbps and starts the smpmgr upload at once.

1. Before the update, write a test value to Homing Offset (register 20, EEPROM area) and read it back.
2. Build a second image that differs only in its boot banner (`-DCONFIG_BOOT_BANNER_STRING="\"Booting update test\""`, [verify] the option in `zephyr/kernel/Kconfig`). Reboot at 1 Mbps, then upload it within the window. Expected: upload completes, reset, RTT shows the new banner.
3. Set Baud Rate to 57600 (code 1), send Reboot. Expected: complete status packet with no error, then the application answers again at 57600 after about 0.5 s plus startup.
4. Start an upload and unplug the USB halfway. Expected: after power-on the application does not start (no RTT line from `main`), a second upload at 1 Mbps succeeds and the application starts.
5. Read register 20 again. Expected: the value from item 1 (storage partition survived).

Report each item as passed or failed with what was seen.

- [ ] **Step 5: Hand over for commit**

Files: `docs/firmware-update.md`, `mkdocs.yml`, `docs/architecture.md`, `docs/development.md`, `docs/hardware-checklist.md`, `docs/simulation.md`, `docs/index.md`, `README.md`, `requirements.txt`. Message: `Document the firmware update over USART2 with MCUboot serial recovery`.
