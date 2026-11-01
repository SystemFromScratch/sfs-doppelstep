# SPDX-License-Identifier: Apache-2.0
#
# Builds the X430 emulator with its own x430.cmake and links it, together with
# DXL2-C, into the Zephyr image.
#
#  - Kconfig: Zephyr's (Kconfig.x430 sourced by ./Kconfig), so x430.cmake runs
#    with X430_KCONFIG=OFF and gets the model and packet size from CONFIG_X430_*;
#    x430_config.h reads the other options from autoconf.h (X430_HAS_AUTOCONF).
#  - DXL2-C: taken from the west project instead of FetchContent
#    (FETCHCONTENT_SOURCE_DIR_DXL2), so the build needs no network access.

if(CONFIG_SFS_DYNAMIXEL_X430)

  get_filename_component(x430_default_dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../modules/lib/dynamixel-x430" ABSOLUTE)
  get_filename_component(dxl2_default_dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../modules/lib/dxl2-c" ABSOLUTE)
  set(X430_SOURCE_DIR "${x430_default_dir}" CACHE PATH "DYNAMIXEL-X430 emulator source tree")
  set(DXL2_SOURCE_DIR "${dxl2_default_dir}" CACHE PATH "DXL2-C source tree")

  foreach(file "${X430_SOURCE_DIR}/x430.cmake" "${DXL2_SOURCE_DIR}/dxl2.cmake")
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "${file} not found. Run `west update` or set X430_SOURCE_DIR / DXL2_SOURCE_DIR.")
    endif()
  endforeach()

  # Configuration of x430.cmake (see the header of that file)
  set(X430_KCONFIG OFF CACHE BOOL "Kconfig comes from Zephyr" FORCE)
  set(X430_MODEL "${CONFIG_X430_MODEL_NAME}" CACHE STRING "Emulated model" FORCE)
  set(X430_DXL2_MAX_PACKET_SIZE "${CONFIG_X430_DXL2_MAX_PACKET_SIZE}" CACHE STRING
      "DXL2_MAX_PACKET_SIZE" FORCE)
  set(FETCHCONTENT_SOURCE_DIR_DXL2 "${DXL2_SOURCE_DIR}" CACHE PATH "DXL2-C west project" FORCE)

  # The DXL2-C revision of west.yml should be the one x430.cmake expects.
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${DXL2_SOURCE_DIR}/.git")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
      WORKING_DIRECTORY "${DXL2_SOURCE_DIR}"
      OUTPUT_VARIABLE dxl2_west_tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(STRINGS "${X430_SOURCE_DIR}/x430.cmake" x430_dxl2_tag_line
         REGEX "set\\(X430_DXL2_GIT_TAG ")
    string(REGEX MATCH "\"(v[0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${x430_dxl2_tag_line}")
    set(x430_dxl2_tag "${CMAKE_MATCH_1}")
    if(dxl2_west_tag AND x430_dxl2_tag AND NOT dxl2_west_tag STREQUAL x430_dxl2_tag)
      message(WARNING "dxl2-c is at ${dxl2_west_tag} (west.yml) but x430.cmake expects "
                      "${x430_dxl2_tag}: update the dxl2-c revision in west.yml.")
    endif()
  endif()

  # Zephyr's CMAKE_MODULE_PATH has its own version.cmake: put DXL2-C's cmake/
  # first so that dxl2.cmake's include(version) finds the right one.
  list(PREPEND CMAKE_MODULE_PATH "${DXL2_SOURCE_DIR}/cmake")

  include("${X430_SOURCE_DIR}/x430.cmake")

  target_compile_definitions(x430 PUBLIC X430_HAS_AUTOCONF)
  target_include_directories(x430 PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

  foreach(lib dxl2 x430)
    target_link_libraries(${lib} PUBLIC zephyr_interface)
    zephyr_append_cmake_library(${lib})
  endforeach()

  message(STATUS "DYNAMIXEL X430 emulator ${X430_VERSION}: model ${X430_MODEL}, "
                 "DXL2-C from ${DXL2_SOURCE_DIR}")

endif()
