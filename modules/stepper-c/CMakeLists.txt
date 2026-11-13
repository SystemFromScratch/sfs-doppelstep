# SPDX-License-Identifier: Apache-2.0
#
# Builds Stepper-C from its west project with its own sstep.cmake and links it
# into the Zephyr image. The build-time options come from Kconfig (CONFIG_SSTEP_*).

if(CONFIG_SSTEP)

  get_filename_component(sstep_default_dir
    "${ZEPHYR_BASE}/../modules/lib/stepper-c" ABSOLUTE)
  set(SSTEP_SOURCE_DIR "${sstep_default_dir}" CACHE PATH "Stepper-C source tree")

  if(NOT EXISTS "${SSTEP_SOURCE_DIR}/sstep.cmake")
    message(FATAL_ERROR
      "Stepper-C not found in ${SSTEP_SOURCE_DIR}. Run `west update` or set SSTEP_SOURCE_DIR.")
  endif()

  if(CONFIG_SSTEP_MATH_FLOAT)
    set(SSTEP_MATH_BACKEND "FLOAT" CACHE STRING "Stepper-C numeric backend" FORCE)
  else()
    set(SSTEP_MATH_BACKEND "FIXED" CACHE STRING "Stepper-C numeric backend" FORCE)
  endif()

  # The SSTEP_CFG_* options (inc/sstep_config.h). sstep.cmake appends the backend to
  # COMPILE_DEFINITIONS and sets them on the library with its interface scope (PUBLIC),
  # so the driver that holds the instance compiles sstep.h with the same values:
  # SSTEP_STATE_BYTES depends on them. COMPILE_OPTIONS would also be PUBLIC, so -O2 is
  # set on the library sources below instead.
  set(COMPILE_DEFINITIONS
    "SSTEP_CFG_TOPOLOGY_FULL_BRIDGE=$<IF:$<BOOL:${CONFIG_SSTEP_TOPOLOGY_FULL_BRIDGE}>,1,0>"
    "SSTEP_CFG_TOPOLOGY_SHARED_LEG=$<IF:$<BOOL:${CONFIG_SSTEP_TOPOLOGY_SHARED_LEG}>,1,0>"
    "SSTEP_CFG_MICROSTEPS_MAX=${CONFIG_SSTEP_MICROSTEPS_MAX}U"
    "SSTEP_CFG_SINE_TABLE_BITS=${CONFIG_SSTEP_SINE_TABLE_BITS}U"
    "SSTEP_CFG_ARG_CHECKS=$<IF:$<BOOL:${CONFIG_SSTEP_ARG_CHECKS}>,1,0>"
    "SSTEP_CFG_SELFTEST=$<IF:$<BOOL:${CONFIG_SSTEP_SELFTEST}>,1,0>"
    "SSTEP_CFG_STALL=$<IF:$<BOOL:${CONFIG_SSTEP_STALL}>,1,0>"
  )

  # Zephyr's CMAKE_MODULE_PATH has its own version.cmake: put the library's
  # cmake/ first so that sstep.cmake's include(version) finds the right one.
  list(PREPEND CMAKE_MODULE_PATH "${SSTEP_SOURCE_DIR}/cmake")

  include("${SSTEP_SOURCE_DIR}/sstep.cmake")

  # Same compiler flags as the kernel (CPU, FPU ABI, warnings, autoconf.h).
  target_link_libraries(sstep PUBLIC zephyr_interface)

  # Source options come after those of zephyr_interface on the command line
  if(CONFIG_SSTEP_OPTIMIZE_SPEED)
    get_target_property(sstep_sources sstep SOURCES)
    set_source_files_properties(${sstep_sources} PROPERTIES COMPILE_OPTIONS -O2)
  endif()

  zephyr_append_cmake_library(sstep)

  message(STATUS "Stepper-C ${SSTEP_VERSION}: ${SSTEP_SOURCE_DIR}, backend ${SSTEP_MATH_BACKEND}")

endif()
