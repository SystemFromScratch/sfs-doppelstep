# Getting started

There are three ways to meet Doppelstep, each one step closer to the real motor. Start with
the first: it needs no hardware, and everything you see there behaves the same way on the
board.

| Path | You need | You get |
|---|---|---|
| [1. On your PC](#1-on-your-pc-no-hardware) | Docker and Git, or a GitHub account (Codespaces) | The firmware as a Linux program with a simulated motor; the tests; a bus you can talk to |
| [2. On the board, simulated motor](#2-on-the-board-simulated-motor) | B-G431B-ESC1 and a USB cable | The real timing (20 kHz interrupt, 1 Mbps UART) with the motor model; the power stage stays off |
| [3. On the board, real motor](#3-on-the-board-real-motor) | Also a two-phase stepper motor and a DC supply (firmware limits 10–28 V, nominal 24 V) | The real thing, after the bring-up checklist |

## Get the source

```bash
git clone https://github.com/SystemFromScratch/sfs-doppelstep.git doppelstep
```

The folder may have any name on your PC; inside the container it is always
`/workdir/doppelstep`. Zephyr and the libraries live in a Docker volume next to it
(`doppelstep-west`), so they survive container rebuilds and keep file access fast on Windows.

## 1. On your PC, no hardware

### With Docker, any operating system

```bash
cd doppelstep
docker compose build                                         # the toolchain image, once
docker compose run --rm zephyr bash scripts/west-init.sh     # Zephyr and the libraries, once
docker compose run --rm zephyr bash scripts/run-tests.sh     # build and run the SIL and integration tests
```

The last command builds the `native_sim` firmware with the simulated motor and runs 28 tests
against it over DYNAMIXEL Protocol 2.0, in about 30 s after the build. The report is in
`build-tests/junit.xml`, the firmware output of every test in `build-tests/pytest/`.

To drive the simulated servo yourself:

```bash
docker compose run --rm zephyr west build -b native_sim/native/64 -s app -d build-native
docker compose run --rm -p 5555:5555 zephyr python3 tools/sim/run_native.py run --tcp 5555
```

The second command starts the firmware in real time and serves its bus on TCP port 5555 of
your PC. In another terminal, on the PC itself:

```bash
python tools/sim/dxl_smoke_test.py --tcp localhost:5555      # ping, torque on, move, read back
```

The smoke test needs only the Python standard library. For ROS 2 on Linux or WSL,
`python3 tools/sim/run_native.py connect --host localhost --port 5555` creates the serial
port `/tmp/ttyDXL`; give that port to `dynamixel_hardware_interface`
([DYNAMIXEL emulation](dynamixel.md#connecting-a-host-program)).

### With VS Code

Open the folder in VS Code and press `Ctrl+Shift+B`: the **Workspace Menu** lists every
action, on Windows and on Linux. The tasks **Zephyr: Run tests (native_sim)** and
**Zephyr: Run (native_sim)** do the steps above. With the Dev Containers extension,
**Dev Containers: Reopen in Container** gives you a shell with `west` ready
([Development environment](development.md)).

### In the browser, with GitHub Codespaces

[Open Doppelstep in GitHub Codespaces](https://codespaces.new/SystemFromScratch/sfs-doppelstep?quickstart=1&devcontainer_path=.devcontainer%2Fcodespaces%2Fdevcontainer.json): the link selects the configuration
**Doppelstep (Codespaces: build and native_sim tests)**. By hand: **Code → Codespaces → … →
New with options**, then that configuration. It starts from the published environment image,
so nothing is downloaded or built at the start. Then run `bash scripts/run-tests.sh` in its
terminal.

## 2. On the board, simulated motor

You need the **B-G431B-ESC1** and a USB cable to its ST-LINK. No motor and no supply are
needed: the motor model replaces the real samples, and the firmware never switches the power
stage on.

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay
west flash -d build -r openocd
```

On Windows, run `west` in the container (`docker compose run --rm zephyr …`), or use the VS
Code tasks **Zephyr: Pristine Build (simulated motor)** and **Zephyr: Flash (OpenOCD,
ST-LINK)**, which use the OpenOCD installed on Windows for flashing.

The board now answers on the ST-LINK Virtual COM Port at 1 Mbps as an XM430-W350 with ID 1.
Try it with DYNAMIXEL Wizard, the [GUI](gui/index.md), or on Linux with the smoke test
(`python3 tools/sim/dxl_smoke_test.py /dev/ttyACM0`). The log, with the rotor
position of the model, comes over RTT: `west rtt -d build -r openocd`.

## 3. On the board, real motor

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build
west flash -d build -r openocd
```

!!! warning "Follow the bring-up checklist first"
    The firmware refuses to switch the power stage on until the dead time of the gate drivers
    is configured (`dead-time-ns`, 0 by default). Wire the motor as described in
    [Hardware and bring-up](hardware.md), then follow its checklist step by step: polarity,
    current loop, motion, protection.

## What a build produces

| File | Use |
|---|---|
| `build/mcuboot/zephyr/zephyr.hex` | MCUboot, the bootloader |
| `build/app/zephyr/zephyr.signed.hex` | The signed application, for a debug probe |
| `build/app/zephyr/zephyr.signed.bin` | The signed application, for an update over the bus ([Firmware update](firmware-update.md)) |
| `build/app/zephyr/zephyr.elf` | Symbols for the debugger |
| `build-native/zephyr/zephyr.exe` | The `native_sim` firmware (Linux program) |

`west flash` loads MCUboot and the signed application together. A pristine build (`-p
always`) is needed after a change to `prj.conf`, Kconfig, the devicetree, `west.yml` or the
toolchain.

## Next steps

| Next | Read |
|---|---|
| Debug with breakpoints, read the log | [Development environment](development.md#flashing-and-debugging) |
| Connect ROS 2 or the OpenMANIPULATOR-X | [DYNAMIXEL emulation](dynamixel.md) |
| Change the motor, the ID, the limits | [Configuration](configuration.md) |
| Understand the code before changing it | [Architecture](architecture.md) |
