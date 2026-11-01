---
name: doppelstep
description: Development and maintenance rules for Doppelstep, the Zephyr RTOS 4.5 firmware for the STMicroelectronics B-G431B-ESC1 that drives a two-phase stepper motor with Stepper-C and emulates a DYNAMIXEL XM430-W350 (Protocol 2.0) with the X430 Control Table Emulator. Covers the Docker/Dev Container environment, the west workspace, the out-of-tree board, the Zephyr module glue of the libraries, the power stage (TIM1/ADC/OPAMP), the execution model, the DYNAMIXEL mapping, flashing and debugging with OpenOCD and J-Link.
---

# Doppelstep development skill

## 1. What the repository is

- West manifest repository **and** Zephyr application (`app/`), out-of-tree
  board (`boards/st/b_g431b_esc1`), devicetree bindings (`dts/bindings`) and the
  Zephyr module glue of three libraries (`modules/`).
- Target: B-G431B-ESC1, STM32G431CBU6, Cortex-M4F at 170 MHz.
- Libraries (west projects, no `zephyr/module.yml` of their own):
  Stepper-C (`modules/lib/stepper-c`, pinned **v1.0.0**),
  DYNAMIXEL-X430 emulator (`modules/lib/dynamixel-x430`, **main** until its
  first release), DXL2-C (`modules/lib/dxl2-c`, the tag `x430.cmake` expects).
- Host side: ROS 2 `dynamixel_hardware_interface` (OpenMANIPULATOR-X), DynamixelSDK,
  DYNAMIXEL Wizard. The firmware must look like a genuine XM430-W350.

## 2. Environment

- `Dockerfile` is the only toolchain definition (python 3.12 slim bookworm by
  digest, west 1.5.0, Zephyr SDK 1.0.1 minimal + `arm-zephyr-eabi` + host
  tools, CMake 3.30.5, PyYAML, pylink-square). Both dev container configs and
  `compose.yaml` use it. Keep every version pinned; verify SDK checksums.
- West topdir `/workdir` is a named volume (`doppelstep-west`) with the
  repository bind-mounted at `/workdir/doppelstep`. Do not move Zephyr
  or modules into the repository.
- `scripts/west-init.sh` = `west init -l` + `west update --narrow --fetch-opt=--tags`
  (tags: the libraries' version scripts use `git describe`).
- SEGGER J-Link Software is never downloaded by the build or committed: only a
  user-provided `docker/jlink/JLink_Linux_V*_x86_64.tgz` is installed.
- Two debug modes, keep both working:
  - A: probe in the container (usbipd-win → WSL 2, `.devcontainer/usb-passthrough`):
    `west flash|debug|debugserver|attach|rtt -r openocd|jlink`.
  - B: GDB server on the host (`scripts/host/*.ps1`), GDB/Cortex-Debug in the
    container via `host.docker.internal:3333` (OpenOCD) / `:2331` (J-Link).

## 3. Build

```bash
west build -b b_g431b_esc1 -s app -d build            # default
west build -p always -b b_g431b_esc1 -s app -d build  # after Kconfig/DT/west.yml/toolchain changes
```

- Deliverable: `build/zephyr/zephyr.elf` (also `.hex`, `.bin`).
- The build must stay free of warnings (including deprecation warnings).
- `app/CMakeLists.txt` adds `BOARD_ROOT`, `DTS_ROOT` and `EXTRA_ZEPHYR_MODULES`;
  do not replace that with environment variables.

## 4. Module glue rules (`modules/*/zephyr`)

- The libraries are built with their **own** CMake entry points
  (`sstep.cmake`, `x430.cmake` → `dxl2.cmake`); do not copy their source lists.
- Library paths default to `<topdir>/modules/lib/<name>` relative to the glue
  (`SSTEP_SOURCE_DIR`, `X430_SOURCE_DIR`, `DXL2_SOURCE_DIR` cache overrides).
- `list(PREPEND CMAKE_MODULE_PATH <lib>/cmake)` before including a library:
  Zephyr has its own `version.cmake`.
- Every library target links `zephyr_interface` (CPU/FPU flags, autoconf) and is
  registered with `zephyr_append_cmake_library()`.
- Stepper-C options come from `CONFIG_SSTEP_*`; set `SSTEP_MATH_BACKEND` as a
  FORCEd cache value. Backend: **FLOAT** (Cortex-M4F).
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

## 6. Execution model (do not break)

| Context | Rule |
|---|---|
| ADC1_2 ISR, zero-latency, priority 0 | `sstep_fast_loop()` and the W trip only; **no kernel calls**, no logging, no locks |
| thread `motor`, cooperative | the **only** caller of `sstep_slow_loop()` and of every other `sstep_*()` |
| thread `dxl` | X430 parser, hooks, events, Bus Watchdog, live registers; motion via `motor_*()` |
| `main` | startup order: power stage → offset calibration (outputs off) → motor → DYNAMIXEL bus |

- `motor_*()` hand commands to the slow loop through a queue and wait for the result.
- The motor publishes a snapshot (spinlock); the bus thread never blocks the motor thread.
- `on_fault` may run in the ISR: atomics only.
- TIM1, ADC1 and ADC2 are programmed with LL; their DT nodes stay `disabled`.
  Configure TIM1 (MOE=0, OSSI=1, idle low) before `pinctrl_apply_state()`.
- FPU: `CONFIG_FPU_SHARING=y`, motor thread created with `K_FP_REGS`.

## 7. DYNAMIXEL mapping rules

- Conversions live in `app/src/joint.c` (pure functions, host-testable): 4096
  pulse/rev, 0.229 rev/min, 214.577 rev/min², 2.69 mA, time-based profile in ms.
- Supported Operating Modes: 1, 3, 4. Others are refused with Data Range Error.
- Present Position = commanded position + start position (2048) + Homing Offset;
  there is no encoder.
- Firmware factory values: ID `CONFIG_SFS_DXL_ID`, Baud code
  `CONFIG_SFS_DXL_FACTORY_BAUD_CODE` (3 = 1 Mbps), applied through
  `x430_device_write()` (validated and stored), never by patching NVM blocks.
- NVM writes are refused while the power stage is enabled.

## 8. Change procedure

1. Read `west.yml`, `Dockerfile`, `app/prj.conf`, `app/Kconfig`, the board DTS
   and `app/boards/b_g431b_esc1.overlay` before changing behaviour.
2. Keep versions pinned (Zephyr tag, Stepper-C tag, SDK, west, pip packages).
3. Smallest change; Zephyr coding style (tabs, 100 columns) in `app/`.
4. Pristine build, no warnings, `zephyr.elf` produced.
5. Hardware-dependent changes: say which bring-up step (README) re-validates them.
6. Update `README.md` (human usage) and this skill (rules) when the workflow or
   architecture changes.

## 9. Definition of done

- Builds pristine without warnings for `b_g431b_esc1`.
- Execution-model rules of section 6 hold.
- No secrets, license files or SEGGER binaries committed.
- Both debug modes and both runners still documented and configured.
- Open hardware items (dead time, NTC, fast-loop timing) are not silently "solved".
