# SPDX-License-Identifier: Apache-2.0
#
# Runners for an SWD probe on J3 (SWDIO, SWCLK, 3V3, GND; no NRST on the
# connector): ST-LINK through OpenOCD or STM32CubeProgrammer, SEGGER J-Link, pyOCD.

# keep first
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=sw")
board_runner_args(pyocd "--target=stm32g431cbux")
board_runner_args(jlink "--device=STM32G431CB" "--speed=4000")

# keep first: `west flash` / `west debug` without -r use OpenOCD
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
