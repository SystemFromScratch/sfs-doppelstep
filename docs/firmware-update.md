# Firmware update

The board updates its firmware over USART2, the UART of the DYNAMIXEL bus. The bootloader is
MCUboot, built by Zephyr sysbuild, and the update uses its serial recovery: an SMP (MCUmgr)
server in the bootloader that writes the new image into the application slot. The application
itself contains no update code.

## Flash layout

STM32G431CB: 128 KiB of flash, 2 KiB pages.

| Partition | Address | Size | Content |
|---|---|---|---|
| `boot_partition` | `0x00000` | 38 KiB | MCUboot |
| `slot0_partition` | `0x09800` | 86 KiB | Application, signed |
| `storage_partition` | `0x1f000` | 4 KiB | NVS: EEPROM area of the emulated DYNAMIXEL |

`boot_partition` is the measured size of MCUboot rounded up to a page. An update never touches
`storage_partition`, so the EEPROM values (ID, Baud Rate, limits) survive it.

## Build and flash

Every build for the board is a sysbuild build:

```bash
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build
west flash -d build -r openocd     # MCUboot and the signed application
```

| File | Content |
|---|---|
| `build/mcuboot/zephyr/zephyr.hex` | MCUboot |
| `build/app/zephyr/zephyr.signed.bin` | Signed application: the file for an update |
| `build/app/zephyr/zephyr.signed.hex` | Signed application for a debug probe |
| `build/app/zephyr/zephyr.elf` | Symbols of the application for the debugger |

Every build from the VS Code tasks or `.vscode/workspace-menu.sh` also copies the images into
`output/` (git-ignored), named with the version from `git describe --tags --dirty`
(`scripts/export_output.py`; `0.1.0` on the tag `v0.1.0`, `0.1.0-3-g1a2b3c4-dirty` after it):

| File in `output/` | Use |
|---|---|
| `doppelstep-<version>-b_g431b_esc1-merged.hex` | MCUboot and the signed application: the first flashing through the ST-LINK |
| `doppelstep-<version>-b_g431b_esc1-app.signed.bin` | The update file for MCUboot (GUI, smpmgr) |
| `doppelstep-<version>-b_g431b_esc1-app.signed.hex`, `-app.elf` | Signed application for a debug probe, symbols |

The simulated-motor build writes the same files with `sim-motor` in place of `app`
(`…-sim-motor-merged.hex`, `…-sim-motor.signed.bin`). Run the script by hand after a plain
`west build`: `python3 scripts/export_output.py --build-dir build`.

A build directory made before MCUboot was added needs one pristine build. The VS Code tasks and
debug configurations already load both images.

## Serial recovery

MCUboot listens on USART2 at **1 Mbps**, 8N1. The rate is fixed: the bootloader does not read
the Baud Rate register of the application.

| Situation | MCUboot |
|---|---|
| Any reset (power-on, NRST, ST-LINK, DYNAMIXEL Reboot) | Waits 500 ms for an SMP request. With a request it stays in serial recovery, without one it starts the application |
| Slot 0 holds no valid image (first flash, interrupted upload) | Stays in serial recovery |

The host must get an SMP request to MCUboot inside the 500 ms window. The window opens only
after the reset and the start of MCUboot, so a request sent right after the status packet of
the Reboot can arrive before MCUboot listens and be lost. A host program therefore sends the
Reboot, then repeats a short SMP request (for example `os echo`) every few tens of milliseconds
until MCUboot answers, and then uploads. A separate smpmgr process started after the Reboot may
start too late: its startup can take longer than 500 ms.

### DYNAMIXEL Reboot on the board

With MCUboot, the Reboot instruction (0x08) resets the MCU after its status packet
(`CONFIG_SFS_DXL_REBOOT_RESETS_MCU`, default on with MCUboot). This differs from an XM430:

- Present Position restarts. The open-loop position lives in RAM and there is no encoder.
- The device does not answer for 500 ms plus the startup time.
- The motor outputs are off before the reset, as with every Reboot.

On `native_sim` Reboot stays emulated, so the SIL tests keep the XM430 behaviour.

## Upload with the GUI

The **Firmware update** tab of the [GUI](gui/index.md) runs the whole sequence in one process:
Reboot, SMP echo repeated until MCUboot answers, image list, upload, image list again, reset, and
a DYNAMIXEL ping of the new application. It passes only when slot 0 holds an image whose hash
equals the SHA-256 TLV of the chosen `zephyr.signed.bin`. MCUboot lists only images that pass its
signature check, so a listed image with that hash is the file, verified.

## Upload with smpmgr

[smpmgr](https://github.com/intercreate/smpmgr) is in `requirements.txt`. The line options
match the SMP buffers of MCUboot (`CONFIG_BOOT_MAX_LINE_INPUT_LEN=128`,
`CONFIG_BOOT_LINE_BUFS=8`).

```bash
smpmgr --port COM5 --baudrate 1000000 --line-length 128 --line-buffers 8 \
    image upload build/app/zephyr/zephyr.signed.bin
smpmgr --port COM5 --baudrate 1000000 --line-length 128 --line-buffers 8 \
    os reset
```

Use `image upload`, not `upgrade`: with one slot there is no test image to mark, and serial
recovery writes slot 0 directly. `COM5` is the ST-LINK Virtual COM Port (Linux:
`/dev/ttyACM0`).

## Signature

MCUboot checks an RSA-2048 signature. The build signs with the development key of MCUboot,
which is public: any image signed with it boots. For a product, generate a key with `imgtool
keygen`, keep it outside the repository and set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` in
`app/sysbuild.conf`. Boards that already run MCUboot with the development key reject images
signed with the new key, so they need MCUboot flashed again through the debug probe.

## Configuration

| What | Where |
|---|---|
| MCUboot on, one slot, signature | `app/sysbuild.conf` |
| Serial recovery, wait window, RAM | `app/sysbuild/mcuboot.conf` |
| USART2 and its rate for MCUboot | `app/sysbuild/mcuboot.overlay` |
| Partitions | `boards/st/b_g431b_esc1/b_g431b_esc1.dts` |
| Reboot resets the MCU | `CONFIG_SFS_DXL_REBOOT_RESETS_MCU` (`drivers/x430/Kconfig`) |

## Troubleshooting

| Symptom | Cause | Remedy |
|---|---|---|
| smpmgr times out, the application answers DYNAMIXEL | No request reached MCUboot inside the 500 ms window | Repeat a short SMP request after the Reboot until MCUboot answers, then upload |
| smpmgr gets no answer at all | The Baud Rate register is not 1 Mbps: the application answers at that rate, MCUboot listens at 1 Mbps | Use `--baudrate 1000000` for smpmgr whatever the register says |
| The application does not start after loading `zephyr.elf` with a debugger | The ELF has no MCUboot header and signature: MCUboot stays in serial recovery | Load `zephyr.signed.hex` (the VS Code configurations do) or upload with smpmgr |
| The application does not start after an interrupted upload | Slot 0 holds no valid image: MCUboot stays in serial recovery | Upload again |
| The GUI: "no SMP answer after N echoes", first update after starting the GUI | Before the fix: smpclient was imported after the Reboot, which took about 0.8 s on Windows, longer than the 500 ms window | Fixed in the GUI: the SMP client is ready before the Reboot |
| MCUboot answers the echo, the upload stops at a chunk (timeout) | MCUboot built before `fifo-enable` was set on its USART2 (`app/sysbuild/mcuboot.overlay`): at 1 Mbps a late receive interrupt drops a byte, and the chunk is lost | MCUboot cannot replace itself over the bus: flash the new `…-merged.hex` once through the ST-LINK |
