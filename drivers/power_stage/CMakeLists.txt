# SPDX-License-Identifier: Apache-2.0

if(CONFIG_POWER_STAGE_STM32 OR CONFIG_POWER_STAGE_SIM)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_POWER_STAGE_STM32 power_stage_stm32.c)
  zephyr_library_sources_ifdef(CONFIG_POWER_STAGE_SIM power_stage_sim.c sim_plant.c)
endif()
