# SPDX-License-Identifier: Apache-2.0
#
# Doppelstep: stepper motor controller on B-G431B-ESC1 that answers as a
# DYNAMIXEL XM430-W350 (Protocol 2.0).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(doppelstep LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/app_msg.c
  src/control_task.c
  src/comm_task.c
  src/joint.c
  src/joint_bridge.c
  src/supervisor.c
)

target_sources_ifdef(CONFIG_POWER_STAGE_SIM app PRIVATE src/sim_log.c)

target_link_libraries(app PRIVATE x430)
