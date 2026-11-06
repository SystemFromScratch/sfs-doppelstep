# SPDX-License-Identifier: Apache-2.0
#
# Runners for the on-board ST-LINK/V2-1 (OpenOCD, STM32CubeProgrammer, pyOCD)
# and for a SEGGER J-Link, including the on-board ST-LINK converted to
# J-Link OB with SEGGER's STLinkReflash utility.

# keep first
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")
board_runner_args(pyocd "--target=stm32g431cbux")
board_runner_args(jlink "--device=STM32G431CB" "--speed=4000")

# keep first: `west flash` / `west debug` without -r use OpenOCD
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
