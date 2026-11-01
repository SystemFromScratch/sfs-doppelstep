# SPDX-License-Identifier: Apache-2.0
#
# Doppelstep: stepper motor controller on B-G431B-ESC1 that answers as a
# DYNAMIXEL XM430-W350 (Protocol 2.0).

cmake_minimum_required(VERSION 3.20.0)

# Repository root: out-of-tree board (boards/), devicetree bindings
# (dts/bindings/) and the Zephyr module glue of the libraries (modules/).
get_filename_component(SFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

list(APPEND BOARD_ROOT "${SFS_ROOT}")
list(APPEND DTS_ROOT "${SFS_ROOT}")
list(APPEND EXTRA_ZEPHYR_MODULES
  "${SFS_ROOT}/modules/stepper-c"
  "${SFS_ROOT}/modules/dynamixel-x430"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(doppelstep LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/power_stage.c
  src/motor.c
  src/joint.c
  src/dxl_bus.c
  src/nvm.c
)

target_link_libraries(app PRIVATE sstep x430)
