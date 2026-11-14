# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module of the repository (zephyr/module.yml). The application is in app/.

add_subdirectory(modules/stepper-c)
add_subdirectory(modules/dynamixel-x430)
add_subdirectory(modules/stepper-lab)

zephyr_include_directories(include)

# Firmware version from git (app/VERSION without git): doppelstep_version.h,
# written at configure time and checked again at every build.
set(DOPPELSTEP_VERSION_DIR ${CMAKE_BINARY_DIR}/doppelstep)
set(doppelstep_version_args
  -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
  -DTEMPLATE=${CMAKE_CURRENT_LIST_DIR}/cmake/doppelstep_version.h.in
  -DOUTPUT=${DOPPELSTEP_VERSION_DIR}/doppelstep_version.h
  -P ${CMAKE_CURRENT_LIST_DIR}/cmake/doppelstep_version.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} ${doppelstep_version_args}
                RESULT_VARIABLE doppelstep_version_result)
if(NOT doppelstep_version_result EQUAL 0)
  message(FATAL_ERROR "cmake/doppelstep_version.cmake failed")
endif()
add_custom_target(doppelstep_version ALL
  COMMAND ${CMAKE_COMMAND} ${doppelstep_version_args}
  BYPRODUCTS ${DOPPELSTEP_VERSION_DIR}/doppelstep_version.h
  COMMENT "Doppelstep version from git")
zephyr_include_directories(${DOPPELSTEP_VERSION_DIR})
# DEVICE_API sections are generated from the __subsystem structs of these directories
zephyr_syscall_include_directories(include)

add_subdirectory(drivers)
