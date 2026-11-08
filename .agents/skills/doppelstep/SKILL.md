---
name: doppelstep
description: Development and maintenance rules for Doppelstep, the Zephyr RTOS 4.5 firmware for the STMicroelectronics B-G431B-ESC1 that drives a two-phase stepper motor with Stepper-C and emulates a DYNAMIXEL XM430-W350 (Protocol 2.0) with the X430 Control Table Emulator. Covers the Docker/Dev Container environment, the west workspace, the repository as a Zephyr module, the out-of-tree board, the power_stage, motor and X430 drivers, the application tasks and queues, the DYNAMIXEL mapping, the simulated motor (on the board and on native_sim), the tests, flashing and debugging with OpenOCD and J-Link.
---

# Doppelstep development skill

## 1. What the repository is

- West manifest repository and Zephyr module (`zephyr/module.yml`), following the Zephyr
  `example-application`: application `app/`, drivers `drivers/` with public APIs in
  `include/doppelstep/drivers/`, bindings `dts/bindings/`, board `boards/st/b_g431b_esc1`, build
  glue of the libraries `modules/<lib>/`.
- Host side of the repository: `tools/gui/` (GUI: test bench and firmware
  update on the board; entry point `tools/gui/doppelstep_gui.py`, package
  `tools/gui/doppelstep_gui/`, whose `protocol.py` is also used by
  `tools/sim/dxl2.py`), `tools/sim/` (native_sim launcher
  `run_native.py`, Protocol 2.0 smoke test `dxl_smoke_test.py`; standard
  library only), `tools/docs/` + `mkdocs.yml` + `docs/` (MkDocs site and PDF
  manual), `requirements.txt` (host Python).
- Targets: B-G431B-ESC1, STM32G431CBU6, Cortex-M4F at 170 MHz; and
  `native_sim/native/64` (the same application as a Linux program, simulated
  motor always on, section 7). Planned next family: TI MSPM0 (`docs/porting.md`).
- Libraries (west projects, no `zephyr/module.yml` of their own):
  Stepper-C (`modules/lib/stepper-c`, pinned **v1.2.0**),
  DYNAMIXEL-X430 emulator (`modules/lib/dynamixel-x430`, **main** until its
  first release), DXL2-C (`modules/lib/dxl2-c`, the tag `x430.cmake` expects),
  Stepper-Lab (`modules/lib/stepper-lab`, **main**: plant model
  `sil/c/sstep_plant.c` and `params/*.json` of the simulated motor) and
  sfs-stepper-power-spice (`modules/lib/sfs-stepper-power-spice`, **main**,
  not built: ngspice source of the stage parameters). Keep the two simulation
  repositories siblings under `modules/lib`.
- Host side: ROS 2 `dynamixel_hardware_interface` (OpenMANIPULATOR-X), DynamixelSDK,
  DYNAMIXEL Wizard. The firmware must look like a genuine XM430-W350.

## 2. Environment

- `Dockerfile` is the only toolchain definition (python 3.12 slim bookworm by
  digest, west 1.5.0, Zephyr SDK 1.0.1 minimal + `arm-zephyr-eabi` + host
  tools, CMake 3.30.5, PyYAML, pylink-square, pytest, the Twister packages). Both dev
  container configs and `compose.yaml` use it. Keep every version pinned; verify SDK
  checksums. Raise the image revision in `compose.yaml` and `.github/workflows/ci.yml` when the
  Dockerfile changes.
- West topdir `/workdir` is a named volume (`doppelstep-west`) with the
  repository bind-mounted at `/workdir/doppelstep`. Do not move Zephyr
  or modules into the repository.
- `scripts/west-init.sh` = `west init -l` + `west update --narrow --fetch-opt=--tags`
  (tags: the libraries' version scripts use `git describe`).
- SEGGER J-Link Software is never downloaded by the build or committed: only a
  user-provided `docker/jlink/JLink_Linux_V*_x86_64.tgz` is installed.
- VS Code tasks (`.vscode/tasks.json`) have a Linux command
  (`.vscode/workspace-menu.sh`, Dev Container or Linux host) and a `windows`
  override (`.vscode/host-menu.ps1`); both take the same action names. On the
  Windows host Zephyr runs in Docker (`docker compose run --rm zephyr bash
  .vscode/workspace-menu.sh "<action>"`). Add a new action to both scripts and
  to the `pickString` inputs, or document why it exists on one platform only.
- Two debug modes, keep both working:
  - A: probe in the container (usbipd-win → WSL 2, `.devcontainer/usb-passthrough`):
    `west flash|debug|debugserver|attach|rtt -r openocd|jlink`.
  - B: GDB server on the host (`scripts/host/*.ps1`), GDB/Cortex-Debug in the
    container via `host.docker.internal:3333` (OpenOCD) / `:2331` (J-Link).

## 3. Build

```bash
west build -b b_g431b_esc1 -s app -d build            # default
west build -p always -b b_g431b_esc1 -s app -d build  # after Kconfig/DT/west.yml/toolchain changes
west build -p always -b b_g431b_esc1 -s app -d build -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay  # simulated motor on the board
west build -b native_sim/native/64 -s app -d build-native                                     # firmware on the PC
```

- Deliverables: `build/zephyr/zephyr.elf` (also `.hex`, `.bin`);
  `build-native/zephyr/zephyr.exe` for native_sim.
- The build must stay free of warnings (including deprecation warnings) in
  all variants. Known exception: host GCC reports
  `-Wmaybe-uninitialized` in the emulator's `x430_bus.c` on native_sim (library
  code: report it to the X430 repository, do not patch it here). Twister turns warnings into
  errors, so a ztest suite must not build that library.
- Configuration: hardware instance parameters are devicetree properties (`app/boards/*.overlay`).
  Kconfig holds compiled features, threads, buffers, timeouts and simulation development options.
  `app/prj.conf` holds only board-independent options; board-specific ones go to
  `app/boards/<board>.conf` (`b_g431b_esc1.conf`: FPU, zero-latency IRQ, OPAMP, RTT, MPU flash
  write; `native_sim_native_64.conf`: 1 ms tick, stdout logs). An option that does
  not exist on every board must never be in `prj.conf` (Kconfig warnings are fatal).

## 4. Module rules

- `zephyr/module.yml` names the repository root as cmake and Kconfig root and as `board_root` and
  `dts_root`. West finds the module because the manifest repository is a project, so applications
  and test applications need no `BOARD_ROOT`, `DTS_ROOT` or `EXTRA_ZEPHYR_MODULES`.
- `dts/bindings/vendor-prefixes.txt` registers `sfs`: Twister rejects an unknown prefix.
- `DEVICE_API` sections come from the `__subsystem` structs of `zephyr_syscall_include_directories`
  (root `CMakeLists.txt`). A new driver class goes in `include/doppelstep/drivers/`.
- The libraries are built with their **own** CMake entry points
  (`sstep.cmake`, `x430.cmake` → `dxl2.cmake`); do not copy their source lists.
- Library paths default to `${ZEPHYR_BASE}/../modules/lib/<name>`
  (`SSTEP_SOURCE_DIR`, `X430_SOURCE_DIR`, `DXL2_SOURCE_DIR`, `STEPPER_LAB_SOURCE_DIR` cache overrides).
- `list(PREPEND CMAKE_MODULE_PATH <lib>/cmake)` before including a library:
  Zephyr has its own `version.cmake`.
- Every library target links `zephyr_interface` (CPU/FPU flags, autoconf) and is
  registered with `zephyr_append_cmake_library()`.
- Stepper-Lab (`modules/stepper-lab`): `CONFIG_SSTEP_PLANT`
  (selected by `CONFIG_POWER_STAGE_SIM`) builds `sstep_plant.c` with the global
  definitions `SSTEP_PLANT_FLOAT=1`, `SSTEP_PLANT_SWITCHING=0` and generates
  `sstep_plant_params.h` from `params/${CONFIG_SSTEP_PLANT_MOTOR_PARAMS}.json`
  and `params/${CONFIG_SSTEP_PLANT_STAGE_PARAMS}.json` (CMake `string(JSON)`,
  `"format": 1` checked, the files are configure dependencies).
- Stepper-C options come from `CONFIG_SSTEP_*`; set `SSTEP_MATH_BACKEND` as a
  FORCEd cache value. Backend default: **FLOAT** with an FPU or on native_sim, otherwise FIXED.
  `CONFIG_SSTEP_OPTIMIZE_SPEED` builds the library with `-O2` through per-source options, which
  come after the optimisation flag of `zephyr_interface`.
- X430: Zephyr's Kconfig sources the emulator's `Kconfig.x430`;
  `x430.cmake` runs with `X430_KCONFIG=OFF`, `X430_MODEL=${CONFIG_X430_MODEL_NAME}`,
  `X430_HAS_AUTOCONF` and the shim `x430_autoconf.h` → `<zephyr/autoconf.h>`.
  DXL2-C comes from the west project (`FETCHCONTENT_SOURCE_DIR_DXL2`): builds are
  offline. Prefer changes in the glue over changes in the libraries; if a
  library change is needed, propose it to the library repository.

## 5. Board and hardware facts

- Source of truth for pins: UM2516 Rev 4, Table 4 (copied into the board DTS
  and `boards/st/b_g431b_esc1/doc/index.rst`). Do not invent pins, channels,
  clock values or analog parameters; ask when a value is not documented.
- HSE 8 MHz → PLL M=2 N=85 R=2 → 170 MHz.
- PWM: TIM1 CH1/CH1N (PA8/PC13) = U, CH2/CH2N (PA9/PA12) = V, CH3/CH3N
  (PA10/PB15) = W. Motor phase A between U and W, phase B between V and W.
- Current sense: OPAMP1/2/3 (PA1,PA3→PA2; PA7,PA5→PA6; PB0,PB2→PB1), PGA gain
  16 with VINM0 as bias ("inverting" in the Zephyr OPAMP binding), effective
  −64/7, 3 mΩ shunts. ADC1_IN3 = iU, ADC2_IN3 = iV, ADC1_IN12 = iW,
  ADC1_IN1 (PA0) = VBUS, divider 18k/(18k+169k).
- **Dead time is not documented in UM2516**: `dead-time-ns` stays 0 (outputs
  never enabled) until the user provides the MCSDK value. Never pick a value.
- USART2 PB3/PB4 = ST-LINK VCP = DYNAMIXEL bus (1 Mbps). Console and logs use RTT.
- Flash: code partition 124 KiB, `storage_partition` 4 KiB (2 pages) for NVS.

## 6. Architecture (do not break)

`docs/architecture.md` is the reference. Four rules, checked by `scripts/check-layers.sh`:

1. Drivers create no threads and no queues; they expose `*_process()`. The fast loop stays in the
   interrupt inside the driver.
2. The X430 driver does not know the motor, and the motor driver does not know DYNAMIXEL. Only
   `app/src/joint_bridge.c` knows both.
3. Only a `power_stage` backend touches registers (LL, DWT, OPAMP, direct interrupts).
4. The devicetree selects the hardware: `power-stage` phandle of the motor, servo as a UART child,
   `storage` phandle. No `#if` on a backend in logic.

| Context | Rule |
|---|---|
| Interrupt of the backend (STM32: ADC1_2, zero latency, priority 0) | Fast loop and the aux trip only; **no kernel calls**, no logging, no locks |
| `control_task`, cooperative | The **only** caller of the motor driver, standard classes included (`CONFIG_SSTEP_MOTOR_THREAD_CHECK`) |
| `comm_task` | `x430_servo_process()`, `joint_bridge`, live registers; motion only through `app_call()` |
| `main` | Startup order: devices ready → offset calibration (outputs off) → `control_task` → `joint_bridge_init` → `comm_task`, then the LED |

- Queues (`app/src/app_msg.h`) carry messages by value. `control_task` never waits on an outgoing
  queue. `app_call()` is bounded and discards late answers by sequence number. `status_q` keeps the
  latest value and is published before a command's answer.
- `on_fault` may run in the interrupt: atomics only. Events reach the application from
  `sstep_motor_process()`.
- Code above the backends assumes no FPU, zero-latency interrupts, cycle counter or on-chip amplifiers
  (`docs/porting.md`). CI runs the motion tests on the fixed-point backend.
- STM32 backend: TIM1, ADC1 and ADC2 are programmed with LL; their DT nodes stay `disabled`.
  Configure TIM1 (MOE=0, OSSI=1, idle low) before `pinctrl_apply_state()`.
- FPU: `CONFIG_FPU_SHARING=y`, `control_task` has `K_FP_REGS` under that option.
- `fast_cycles_max` (DWT cycle counter, `power_stage_get_stats`) measures the fast-loop
  callback on the board; keep it outside the callback's own work.

## 7. Simulated motor

- The model is **not** code of this repository: `sil/c/sstep_plant.c` of
  Stepper-Lab (the code of its Xcos plant block) built in single
  precision with the averaged stage, open windings when the outputs are off,
  and the ngspice leg voltage error table (`CONFIG_SFS_SIM_LEG_ERROR`).
  `drivers/power_stage/sim_plant.[ch]` is only the Zephyr adapter (ADC scaling from
  `power_stage_info`, publication). `app/src/sim_log.c` writes the log line, because it needs the
  commanded position; the tests parse it, so keep its format and the module name `sim_plant`.
  Change the model or its parameters in Stepper-Lab / sfs-stepper-power-spice, never by copying it
  here; the switching stage and the ngspice simulations stay offline.
- Parameters: the JSON files of Stepper-Lab; `CONFIG_SFS_SIM_*` motor
  values are overrides (`-1` = file value). User documentation: `docs/simulation.md`.
- Backend `sfs,sim-power-stage`, selected by the `power-stage` phandle of the motor. Sequence per PWM
  period, in the fast-loop context: `sim_plant_sample()` → fast-loop callback → `sim_plant_step()`.
- **A, on the board** (`app/sim.overlay`, `timebase = <&power_stage>`):
  TIM1/ADC keep the real timing and scaling; the samples come from the model. The backend never calls
  `power_stage_output_enable()` on the real stage, so **MOE is never set** in this mode, whatever the
  dead time: this is the safety rule of the mode, `check-layers.sh` guards it, never weaken it.
- **B, native_sim** (no `timebase`): scaling from the node properties, the ESC1 values: keep them
  equal to `b_g431b_esc1.overlay`. A `k_timer` every 1 ms tick runs the 20 fast-loop periods of the
  tick, then the slow loop runs: the 20:1 ratio of the MCU, host wakes once per ms. Bus on `uart1` (PTY);
  `run_native.py run` links it to `build-native/ttyDXL` and, in Docker, serves
  TCP 5555 (`connect` makes `/tmp/ttyDXL` on the ROS 2 machine). The PTY driver
  polls the first RX byte every 10 ms. NVS lives in `build-native/flash.bin`.
- The fast-loop side of the model makes no kernel calls; threads read the state through
  `power_stage_sim_get_state()` (sequence lock). A redefined position reaches the model through
  `power_stage_position_shifted()`.
- Present Position / Present Velocity stay the commanded values in both
  modes (as with the real motor); the rotor position of the model is in the
  log only. Load torque is a constant active torque (gravity-like).
- The rotor of the model is never exactly still: one ADC step in the current loop excites the
  mechanical resonance (about 125 Hz at the hold current, up to about 3 rpm). Never assert 0 rpm: use
  `support.assert_stopped()`. Not measured on hardware (`docs/hardware-checklist.md`).
- Code takes no simulated time on native_sim: interrupt load and priorities are validated only with
  mode A. The simulated time can run ahead of the host clock: measure durations with the Realtime
  Tick register, not `time.monotonic()`.

- Tests (`docs/testing.md`): ztest suites in `tests/app` and `tests/drivers` (Twister);
  `tests/sil` (pytest + `tools/sim/dxl2.py`) and `tests/integration` (DynamixelSDK, ros2_control loop)
  against the `native_sim` firmware built with `sim-test.conf` (model log every 100 ms).
  Every test gets its own `zephyr.exe` and `flash.bin`; scenarios through
  `-sim_load_unm` / `-sim_vbus_mv` (`power_stage_sim.c`). Assert real
  behaviour (measure first, then fix the numbers from physics or the
  e-Manual); a known gap is an `xfail(strict=True)` with the reason, never a
  deleted test. Load acts from the first Torque Enable. After a timing change, run a suspicious test
  repeatedly and alone too: the simulation is nearly deterministic, so a fragile test can pass every
  time in one order.

## 8. DYNAMIXEL mapping rules

- Conversions live in `app/src/joint.c` (pure functions, host-testable): 4096
  pulse/rev, 0.229 rev/min, 214.577 rev/min², 2.69 mA, time-based profile in ms.
- Supported Operating Modes: 1, 3, 4. Others are refused with Data Range Error.
- Present Position = commanded position + `start-position` (2048) + Homing Offset;
  there is no encoder.
- Firmware factory values: `factory-id`, `factory-baud-code` (3 = 1 Mbps) of the servo node, applied
  through `x430_device_write()` (validated and stored), never by patching NVM blocks.
- NVM writes are refused while the power stage is enabled (`nvm_allowed` callback).

## 9. Change procedure

1. Read `west.yml`, `Dockerfile`, `app/prj.conf`, `app/Kconfig`, the board DTS and
   `app/boards/b_g431b_esc1.overlay` before changing behaviour.
2. Keep versions pinned (Zephyr tag, Stepper-C tag, SDK, west, pip packages).
3. Smallest change; Zephyr coding style (tabs, 100 columns) in `app/` and `drivers/`.
4. `docker compose run --rm zephyr bash scripts/ci.sh` passes: layer check, pristine
   builds without warnings (ESC1, ESC1 + `sim.overlay`, native_sim), ztest suites, SIL /
   integration tests, motion tests on the fixed-point backend. New behaviour of the bus, motion or
   protection path gets a test in `tests/sil` (or `tests/integration` for host-library behaviour),
   and new driver behaviour a ztest case.
5. Hardware-dependent changes: say which bring-up step (docs/hardware.md) or check of
   `docs/hardware-checklist.md` re-validates them.
   Changes of the simulated motor model: in Stepper-Lab (`sstep_plant.c`
   and the Scilab prototype together, `tests/run_all.sce` S01–S05), then
   `west update` here; try them first with `-DSTEPPER_LAB_SOURCE_DIR=<checkout>`.
6. Update `README.md` (project overview and quick start), `docs/` (MkDocs site and PDF manual) and
   this skill (rules) when the workflow or architecture changes; `mkdocs build`
   is strict and must pass.

## 10. Definition of done

- `scripts/ci.sh` passes (the same script as GitHub Actions).
- The architecture rules of section 6 and simulated-motor rules of section 7 hold.
- No secrets, license files or SEGGER binaries committed.
- Both debug modes and both runners still documented and configured.
- Open hardware items (dead time, NTC, fast-loop timing, rotor vibration) are not silently "solved".
