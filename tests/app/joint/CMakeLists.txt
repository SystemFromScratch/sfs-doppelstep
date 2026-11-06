# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_joint LANGUAGES C)

target_sources(app PRIVATE src/main.c ../../../app/src/joint.c)
target_include_directories(app PRIVATE ../../../app/src)
target_link_libraries(app PRIVATE x430)
