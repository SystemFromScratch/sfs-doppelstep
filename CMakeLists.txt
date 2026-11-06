# SPDX-License-Identifier: Apache-2.0
#
# Zephyr module of the repository (zephyr/module.yml). The application is in app/.

add_subdirectory(modules/stepper-c)
add_subdirectory(modules/dynamixel-x430)
add_subdirectory(modules/stepper-lab)

zephyr_include_directories(include)
# DEVICE_API sections are generated from the __subsystem structs of these directories
zephyr_syscall_include_directories(include)

add_subdirectory(drivers)
