# Configuration

Doppelstep is configured the Zephyr way. The **devicetree** describes the hardware: the
power stage, the motor and its limits, the servo on its UART. **Kconfig** selects the
compiled features, the threads and the buffers. Both are read at build time, so a change
needs a rebuild (a pristine one for the devicetree and Kconfig). Settings that a host may
change at run time, such as the ID or the baud rate, are servo registers
([DYNAMIXEL emulation](dynamixel.md)).

## Where each setting lives

| What | Where |
|---|---|
| Motor (R, L, rated current, back-EMF, steps), stage limits, control (microsteps, bandwidth, currents, velocity, acceleration) | Motor node, `app/boards/<board>.overlay` |
| PWM frequency, dead time, ADC channels, shunt, amplifier gain, VBUS divider | Power stage node, `app/boards/b_g431b_esc1.overlay` |
| DYNAMIXEL ID, factory baud, start position, gear, NVS partition | Servo node, a child of the UART |
| Threads, queues, offset samples, command timeout | `CONFIG_SFS_*`, `app/Kconfig` |
| Stepper-C build options (backend, topology, sine table, `-O2`) | `CONFIG_SSTEP_*`, `modules/stepper-c/Kconfig` |
| X430 model, devices per bus, protocol features | `CONFIG_X430_*` (Kconfig of the emulator) |
| Bus buffer, poll period, persistent storage, Reboot behaviour | `CONFIG_SFS_DXL_*`, `drivers/x430/Kconfig` |
| Simulated motor (model parameters, scenario, log) | `CONFIG_SFS_SIM_*`, `app/sim.overlay` ([Simulated motor](simulation.md#parameters)) |
| Board-specific Zephyr options (FPU, RTT, OPAMP; `native_sim` tick and logs) | `app/boards/<board>.conf` |
| Bootloader, flash layout, serial recovery | `app/sysbuild.conf`, `app/sysbuild/`, partitions in the board devicetree ([Firmware update](firmware-update.md#configuration)) |

Open the Kconfig menu of a build with `west build -d build -t menuconfig`.

## The motor node

The values for the 17HS19-2004S1 motor and the B-G431B-ESC1, from
`app/boards/b_g431b_esc1.overlay`:

| Property | Value | Meaning |
|---|---|---|
| `phase-resistance-milliohm` | 1600 | Coil resistance (datasheet) |
| `phase-inductance-microhenry` | 3000 | Coil inductance (datasheet) |
| `rated-current-milliamp` | 2000 | Rated phase current (datasheet) |
| `back-emf-uv-s-per-rad` | 208600 | Back-EMF constant |
| `full-steps-per-rev` | 200 | Full steps per revolution |
| `vbus-nominal-mv`, `vbus-min-mv`, `vbus-max-mv` | 24000, 10000, 28000 | Supply voltage: nominal and trip limits |
| `trip-current-milliamp` | 3500 | Overcurrent trip |
| `aux-trip` | set | Also trip on the shared leg W |
| `slow-loop-hz` | 1000 | Motion loop rate; the PWM rate must be an integer multiple |
| `microsteps` | 16 | Microsteps per full step |
| `current-bandwidth-hz` | 1000 | Bandwidth of the current controller |
| `run-current-milliamp`, `hold-current-milliamp` | 1500, 500 | Current while moving and at rest |
| `hold-delay-ms` | 500 | Time at rest before the hold current applies |
| `max-velocity`, `max-acceleration` | 16000, 64000 | Upper limits of every motion profile |

## The power stage node

| Property | Value | Meaning |
|---|---|---|
| `pwm-frequency` | 20000 | PWM and fast-loop rate, Hz |
| `dead-time-ns` | 0 | Dead time of the half-bridges. **Must be set before the outputs can be enabled** ([bring-up](hardware.md#bring-up-checklist)) |
| `vbus-divider` | 18000, 169000 | VBUS divider resistors, Ω |
| `current-sense-invert` | not set | Inverts the current measurement, if bring-up step 3 requires it |

Shunt, amplifier gain and the ADC channels are in the same node; their values and the
resulting full scale are in [Hardware and bring-up](hardware.md#current-and-voltage-scaling).

## The servo node

| Property | Default | Meaning |
|---|---|---|
| `factory-id` | 1 | DYNAMIXEL ID while no EEPROM is stored and after Factory Reset |
| `factory-baud-code` | 3 (1 Mbps) | Baud Rate register value, same conditions |
| `start-position` | 2048 | Present Position at power-on (no encoder) |
| `gear-motor-revs`, `gear-joint-revs` | 1, 1 | Gear ratio between motor and joint |
| `storage` | `&storage_partition` | Flash partition of the EEPROM area (NVS) |

## Variants of the build

| Variant | Command |
|---|---|
| Board, real motor | `west build -p always --sysbuild -b b_g431b_esc1 -s app -d build` |
| Board, simulated motor | add `-- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay` |
| PC (`native_sim`) | `west build -b native_sim/native/64 -s app -d build-native` |
| Fixed-point Stepper-C backend | add `-- -DCONFIG_SSTEP_MATH_FIXED=y` |

`sim.overlay` stays in the CMake cache of a build directory, so a following incremental build
keeps the simulated motor until the next pristine build.
