# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_joint LANGUAGES C)

target_sources(app PRIVATE src/main.c ../../../app/src/joint.c)
# Only the register definitions of the emulator, not the library
target_include_directories(app PRIVATE ../../../app/src
  ${ZEPHYR_BASE}/../modules/lib/dynamixel-x430/inc)
