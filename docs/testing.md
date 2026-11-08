# Tests

Three levels run on the PC: ztest suites for the parts, and SIL and integration tests for the whole
firmware. [Checks on the board](hardware-checklist.md) cover what a PC cannot see.

## ztest suites

| Suite | Checks |
|---|---|
| `tests/app/joint` | Unit conversions and motion profiles |
| `tests/app/msg` | Queues: timeout, discarded late answer, full queue, latest status |
| `tests/drivers/power_stage` | API contract of the class on the simulation backend, `-ENOSYS` of optional functions |
| `tests/drivers/sstep_motor` | Motor driver through the standard classes, on a fake power stage |

```bash
west twister -T tests/app -T tests/drivers -p native_sim/native/64 --integration
```

`tests/drivers/common/` holds the fake power stage: the test fires the fast loop itself, so every
step is deterministic.

## SIL and integration tests

The whole firmware runs on the PC as a `native_sim` program, with the motor model of
[the simulated motor](simulation.md) behind the power stage. Pytest drives it over
DYNAMIXEL Protocol 2.0, the way a host program does. The tests run in the Docker image,
both on a developer PC and in GitHub Actions, with the same command.

```text
pytest ──Protocol 2.0──► pseudoterminal ──► zephyr.exe (native_sim, real time)
   │                                          X430 emulator → Stepper-C → sstep_plant.c
   └──── model log every 100 ms (rotor position, speed, currents) ◄──┘
```

## Running them

| Where | Command |
|---|---|
| Windows host | VS Code task **Zephyr: Run tests (native_sim)**, or `docker compose run --rm zephyr bash scripts/run-tests.sh` |
| Dev Container / Linux | `bash scripts/run-tests.sh` (task of the same name) |
| Like CI (builds + tests) | `docker compose run --rm zephyr bash scripts/ci.sh` |
| GitHub | `.github/workflows/ci.yml` on every push to `main` and every pull request |

`scripts/run-tests.sh` builds `build-tests/` (`native_sim/native/64` with
`app/sim-test.conf`, which logs the model every 100 ms) and runs pytest.
Arguments go to pytest:

```bash
bash scripts/run-tests.sh tests/sil                 # only the SIL tests
bash scripts/run-tests.sh -k protection -x          # a subset, stop at the first failure
TESTS_PRISTINE=1 bash scripts/run-tests.sh          # pristine build first
STEPPER_LAB_SOURCE_DIR=/sim bash scripts/run-tests.sh # unpublished Stepper-Lab checkout
TESTS_BUILD_DIR=build-tests-fixed TESTS_EXTRA_CMAKE_ARGS=-DCONFIG_SSTEP_MATH_FIXED=y \
    bash scripts/run-tests.sh tests/sil/test_motion.py # fixed-point backend
```

The results are written to the build directory:

- `build-tests/junit.xml`: the report.
- `build-tests/pytest/<test>/fw.log`: the complete firmware output of every test.

The full suite takes about 30 s.

### Levels

| Level | Directory | What runs | Checks |
|---|---|---|---|
| **SIL** | `tests/sil/` | the firmware with the plant model; client `tools/sim/dxl2.py` (raw Protocol 2.0, error byte, malformed packets) | Control Table behaviour, and the effect on the rotor of the model |
| **Integration** | `tests/integration/` | the official **DynamixelSDK** (`dynamixel-sdk`), the library under the ROS 2 `dynamixel_hardware_interface` | the ros2_control loop: Sync Read of current, velocity and position, Sync Write of the goal at 100 Hz |
| Hardware (manual) | [Bring-up checklist](hardware.md#bring-up-checklist) | the board with a motor | dead time, polarity, current loop |

The firmware and model checks of other repositories are separate. Stepper-Lab runs
the Xcos SIL against the Scilab prototype (S01–S05), and sfs-stepper-power-spice runs
the ngspice benches.

### What the tests cover

| File | Tests |
|---|---|
| `sil/test_boot.py` | clean startup, ping (model 1020), live registers at rest (24.0 V, position 2048), factory EEPROM |
| `sil/test_motion.py` | Goal Position moves the rotor of the model to the commanded position; run and hold current; Profile Velocity; time-based profile (Drive Mode 4, OpenMANIPULATOR-X); velocity mode; torque off releases the motor; refused Operating Mode; EEPROM locked while the torque is on |
| `sil/test_protection.py` | load angle against `T = Kt·I·sin(δ)` at the run and the hold current; overload (0.4 N·m), which trips with Hardware Error Status bit 4, the Alert bit and torque off, all cleared by Reboot; undervoltage (9 V), which sets bit 0; Torque Enable in a fault is refused; **lost synchronism, which is not reported** (expected failure, see below) |
| `sil/test_bus.py` | bad CRC and other IDs get no answer; Bus Watchdog stops the motor and refuses goals until cleared; Reboot; Factory Reset; Sync Read / Write; rapid goals during a move; Clear Multi-Turn during a move is refused; garbage on the bus |
| `sil/test_eeprom.py` | EEPROM values survive a power cycle (same `flash.bin`), RAM values do not |
| `integration/test_dynamixel_sdk.py` | SDK ping, read and write; 3 s of the ros2_control loop on a sine trajectory: no failed transaction, more than 50 Hz, tracking error within a few cycles, rotor synchronous |

#### Expected failure: lost synchronism

`test_lost_synchronism_is_reported` is marked `xfail(strict=True)`. A load of
0.12 N·m is held at the run current (`Kt·1.5 A = 0.31 N·m`) but not at the hold current
(`Kt·0.5 A = 0.10 N·m`), so the joint falls. The firmware still reports Torque Enable 1,
no hardware error and the commanded position. A stepper motor without an encoder or
stall detection cannot see this. The test documents the gap. When stall detection is
added, the test starts to pass, and pytest reports the unexpected pass until the marker
is removed.

### Scenario of a test

Each test starts its own `zephyr.exe` in a fresh directory, so it gets a new emulated
EEPROM (`flash.bin`). Options of the executable set the scenario without a rebuild:

| Option | Effect |
|---|---|
| `-sim_load_unm=<µN·m>` | load torque (gravity-like), acting from the first Torque Enable |
| `-sim_vbus_mv=<mV>` | bus voltage of the model |

Example:

```python
def test_something(firmware_factory):
    fw = firmware_factory(["-sim_load_unm=80000"])
    bus = dxl2.Bus(fw.pty)
    bus.write(DXL_ID, dxl2.TORQUE_ENABLE, 1)
    s = fw.next_sample()           # rotor, lag (electrical degrees), rpm, currents
```

Fixtures (`tests/conftest.py`):

- `fw`: the firmware with the default scenario.
- `bus`: a `dxl2.Bus` on it.
- `firmware_factory(args)`: firmware with a scenario.

`fw.restart()` keeps the working directory, so the EEPROM survives.
`support.wait_until()` polls with a timeout. `support.assert_stopped()` checks a stopped motor:
the commanded position stands still and the rotor stays under 5 rpm, because the rotor of the model
vibrates at rest ([why](simulation.md#limits)). Times that matter come from the Realtime Tick
register, because the simulated time of `native_sim` can run ahead of the host clock.

The firmware runs in real time, so the bus timing is the one a host program sees. The
pseudoterminal driver polls for the first byte of a packet every 10 ms.

## CI

`scripts/ci.sh` runs the same steps on a PC and in GitHub Actions:

1. `scripts/check-layers.sh`: the dependency rules of [the architecture](architecture.md#four-rules).
2. `west update` (skipped in GitHub Actions, `WEST_UPDATE=0`: the image holds the workspace).
3. Pristine builds without warnings of `b_g431b_esc1`, `b_g431b_esc1` with `sim.overlay`,
   and the `native_sim` test firmware. The only known warning is the X430 emulator's
   `-Wmaybe-uninitialized` under host gcc, which is reported upstream.
4. The ztest suites, with Twister.
5. The SIL and integration tests.
6. The SIL motion tests on the Stepper-C fixed-point backend.

In GitHub Actions `scripts/ci.sh` runs in the environment image (tools and the west workspace,
[development environment](development.md#ci-and-the-environment-image)); ccache is kept in the
GitHub Actions cache.

It uploads the test reports, the firmware logs and the ESC1 images, and writes a test table
to the job summary.

The pinned test packages are in `tests/requirements.txt` and in the `Dockerfile`
(`ARG PYTEST_VERSION ...`). Keep both in step. For the local image, raise the revision in
`compose.yaml` (`doppelstep:1.0.1-<n>`) when the Dockerfile changes; the CI image gets a new tag
by itself.
