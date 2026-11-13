# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../common")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sstep_motor LANGUAGES C)

# The motor suite fires every sample itself; with the self-test on, the load
# model of src/selftest.c fires them from a timer.
if(CONFIG_SSTEP_MOTOR_SELFTEST)
  target_sources(app PRIVATE src/selftest.c ../common/fake_power_stage.c)
else()
  target_sources(app PRIVATE src/main.c ../common/fake_power_stage.c)
endif()
target_include_directories(app PRIVATE ../common)
