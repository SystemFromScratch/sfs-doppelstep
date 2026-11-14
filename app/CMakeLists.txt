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

# doppelstep_config.h: factory Control Table and driver data of this build, generated
# from the devicetree (it changes only at configure time). Look, do not edit.
set(doppelstep_config_h ${CMAKE_BINARY_DIR}/doppelstep/doppelstep_config.h)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env ZEPHYR_BASE=${ZEPHYR_BASE}
          ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../scripts/gen_doppelstep_config.py
          --edt-pickle ${EDT_PICKLE}
          --x430-dir ${X430_SOURCE_DIR}
          --model ${CONFIG_X430_MODEL_NAME}
          --overrides "${X430_MODEL_OVERRIDES}"
          --output ${doppelstep_config_h}
  RESULT_VARIABLE doppelstep_config_result)
if(NOT doppelstep_config_result EQUAL 0)
  message(FATAL_ERROR "scripts/gen_doppelstep_config.py failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_LIST_DIR}/../scripts/gen_doppelstep_config.py)
message(STATUS "Doppelstep factory values: ${doppelstep_config_h}")
# doppelstep_version.h (root CMakeLists.txt): the version in the startup log
add_dependencies(app doppelstep_version)
