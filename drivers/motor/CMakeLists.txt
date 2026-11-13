# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(sstep_motor.c)
zephyr_library_sources_ifdef(CONFIG_SSTEP_MOTOR_MCUVIEWER sstep_mcuviewer.c)
zephyr_library_link_libraries(sstep)
