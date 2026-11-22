# Supported boards

Doppelstep runs on two motor-control boards and on a PC. The firmware above the power stage
is the same everywhere. A board differs in its power stage backend, chosen by the
devicetree, and in what it carries besides the bridge: an encoder, a status LED.

## Compare the boards

| | B-G431B-ESC1 | Full-bridge board (DRV8412) | PC (`native_sim`) |
|---|---|---|---|
| Zephyr board | `b_g431b_esc1` | `sfs_doppelstep_full_bridge` | `native_sim/native/64` |
| MCU | STM32G431CB, Cortex-M4F, 170 MHz | STM32G431CB, Cortex-M4F, 170 MHz | Linux program |
| Power stage | Three half-bridges, shared-leg topology (L6387, STL180N6F7) | Two full H-bridges in a TI DRV8412 | Motor model of Stepper-Lab with ngspice stage values |
| Current sensing | Three 3 mΩ low-side shunts, internal OPAMP1..3 | Two 10 mΩ low-side shunts, OPAMP1/2 with an external gain of 25.5 | Model |
| Hardware trips | Short circuit on U, V, W and bus overvoltage (comparators, TIM1 break) | Bus overvoltage; the DRV8412 limits the current and reports FAULT and OTW | — |
| Encoder | None: open loop | MPS MA730 on SPI2 | Optional simulated encoder (`-sim_encoder`) |
| STATUS LED | GPIO LED on PC6 | First WS2812B of the chain on PB3 | — |
| DYNAMIXEL bus | USART2 on PB3/PB4 and the ST-LINK Virtual COM Port; single wire on PB3 as an option | USART3 on J4 | TCP socket or pseudo terminal (`tools/sim/run_native.py`) |
| Status | Checked with a real motor at 12 V and 24 V | Built and tested in CI; not yet run on hardware | SIL and integration tests on every CI run |

Both STM32 boards also run with the [simulated motor](../simulation.md) instead of a real one
(`sim.overlay`).

## Board pages

| Board | Doppelstep on the board | Board reference |
|---|---|---|
| B-G431B-ESC1 | [Hardware and bring-up](../hardware.md): motor wiring, scaling, bring-up | [Pinout, flash, debugging](b_g431b_esc1.md) |
| Full-bridge board (DRV8412) | [Hardware and bring-up](../hardware-full-bridge.md): origin and terms of use, wiring, backend, encoder, bring-up | [Pinout, flash, debugging](sfs_doppelstep_full_bridge.md) |

The board reference pages are the Zephyr board documentation of this repository,
`boards/<vendor>/<board>/doc/index.rst`, converted for this website at build time.

## Build for a board

```bash
# B-G431B-ESC1, real motor; add -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay for the simulated one
west build -p always --sysbuild -b b_g431b_esc1 -s app -d build
# Full-bridge board
west build -p always --sysbuild -b sfs_doppelstep_full_bridge -s app -d build-fb
# PC
west build -b native_sim/native/64 -s app -d build-native
```

`--sysbuild` builds MCUboot and the signed application together
([Firmware update](../firmware-update.md)). All build variants: [Configuration](../configuration.md#variants-of-the-build).

## Add a board

1. **Describe the board.** For a board that Zephyr does not have, add
   `boards/<vendor>/<board>/` with `board.yml`, the devicetree, the defconfig and the runners,
   and its documentation `doc/index.rst`: the board reference page appears on this website.
   Add the page to the `nav` of `mkdocs.yml`. Give the UART of the bus the node label
   `sfs_dxl_uart` for MCUboot serial recovery.
2. **Describe the application nodes.** In `app/boards/<board>.overlay`: the power stage node,
   the motor and the servo, and the `chosen` nodes `sfs,motor` and `sfs,servo`; optionally
   `sfs,encoder` and `sfs,status-led-strip` ([Configuration](../configuration.md#chosen-nodes)).
   Board-specific Kconfig options go into `app/boards/<board>.conf`.
3. **Check the power stage backend.** An STM32G4 board with three half-bridges or with a
   gate driver like the DRV8412 uses an existing backend. Another MCU family needs a backend
   of its own ([Porting to a new MCU family](../porting.md)).
4. **Bring it up.** Follow the [hardware checklist](../hardware-checklist.md), measure the
   dead time ([Dead time](../dead-time.md), [Measurements](../measurements.md)), and add the
   build to `scripts/ci.sh`.
