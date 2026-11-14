# SPDX-License-Identifier: Apache-2.0
#
# Version of the Doppelstep firmware, from git, into doppelstep_version.h.
#
# Runs at every build (cmake -P, from the root CMakeLists.txt), so a new commit or
# tag updates the header without a new CMake configuration; configure_file()
# rewrites the header only when its content changes, so nothing is rebuilt
# otherwise.
#
#   cmake -DSOURCE_DIR=<repository> -DTEMPLATE=<.h.in> -DOUTPUT=<.h> -P doppelstep_version.cmake
#
# Source of the version:
#   1. `git describe --tags --long --dirty --match v*.*.*`: vMAJOR.MINOR.PATCH of the
#      last release tag, the commits after it and the -dirty mark.
#   2. Without git or without a tag (a source archive, a shallow clone): app/VERSION,
#      0 commits. app/VERSION is also the version of the MCUboot image header; the
#      release pipeline checks that it equals the tag (scripts/package_release.py),
#      and this script warns when the two differ.

foreach(var SOURCE_DIR TEMPLATE OUTPUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "doppelstep_version.cmake: ${var} not set")
  endif()
endforeach()

# --- app/VERSION ------------------------------------------------------------------
file(STRINGS "${SOURCE_DIR}/app/VERSION" version_lines)
foreach(field VERSION_MAJOR VERSION_MINOR PATCHLEVEL)
  set(file_${field} "")
  foreach(line IN LISTS version_lines)
    if(line MATCHES "^[ \t]*${field}[ \t]*=[ \t]*([0-9]+)")
      set(file_${field} "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(file_${field} STREQUAL "")
    message(FATAL_ERROR "app/VERSION has no ${field}")
  endif()
endforeach()
set(file_version "${file_VERSION_MAJOR}.${file_VERSION_MINOR}.${file_PATCHLEVEL}")

# --- git describe ------------------------------------------------------------------
set(describe "")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --long --dirty --match "v[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE describe_result)
  if(NOT describe_result EQUAL 0)
    set(describe "")
  endif()
endif()

if(describe MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$")
  set(major "${CMAKE_MATCH_1}")
  set(minor "${CMAKE_MATCH_2}")
  set(patch "${CMAKE_MATCH_3}")
  set(commits "${CMAKE_MATCH_4}")
  set(hash "${CMAKE_MATCH_5}")
  if(CMAKE_MATCH_6)
    set(dirty 1)
  else()
    set(dirty 0)
  endif()
  set(source "git")
  if(NOT "${major}.${minor}.${patch}" STREQUAL file_version)
    message(WARNING "Doppelstep version: the last tag is v${major}.${minor}.${patch}, "
                    "app/VERSION says ${file_version} (MCUboot reads app/VERSION). "
                    "Tag the release or update app/VERSION.")
  endif()
else()
  set(major "${file_VERSION_MAJOR}")
  set(minor "${file_VERSION_MINOR}")
  set(patch "${file_PATCHLEVEL}")
  set(commits 0)
  set(hash "")
  set(dirty 0)
  set(source "app/VERSION")
endif()

foreach(part major minor patch)
  if(${part} GREATER 255)
    message(FATAL_ERROR "Doppelstep version: ${part} ${${part}} does not fit one byte "
                        "of Model Information")
  endif()
endforeach()

# Version string: "0.2.0" on the tag, "0.2.0+3.gabc1234" 3 commits later,
# "-dirty" with uncommitted changes (SemVer build metadata).
set(DOPPELSTEP_VERSION_STRING "${major}.${minor}.${patch}")
if(commits GREATER 0 OR dirty)
  set(DOPPELSTEP_VERSION_STRING "${DOPPELSTEP_VERSION_STRING}+${commits}")
  if(NOT hash STREQUAL "")
    set(DOPPELSTEP_VERSION_STRING "${DOPPELSTEP_VERSION_STRING}.g${hash}")
  endif()
endif()
if(dirty)
  set(DOPPELSTEP_VERSION_STRING "${DOPPELSTEP_VERSION_STRING}-dirty")
endif()

set(DOPPELSTEP_VERSION_MAJOR ${major})
set(DOPPELSTEP_VERSION_MINOR ${minor})
set(DOPPELSTEP_VERSION_PATCH ${patch})
set(DOPPELSTEP_VERSION_COMMITS ${commits})
set(DOPPELSTEP_VERSION_DIRTY ${dirty})
set(DOPPELSTEP_VERSION_SOURCE ${source})

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
