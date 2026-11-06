# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(x430_servo.c x430_nvm.c)
zephyr_library_link_libraries(x430)
