# SPDX-License-Identifier: Apache-2.0

if(CONFIG_POWER_STAGE_STM32 OR CONFIG_POWER_STAGE_STM32_FULL_BRIDGE OR CONFIG_POWER_STAGE_SIM)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_POWER_STAGE_STM32 power_stage_stm32.c stm32_pwm_adc.c)
  zephyr_library_sources_ifdef(CONFIG_POWER_STAGE_STM32_FULL_BRIDGE
    power_stage_stm32_full_bridge.c stm32_pwm_adc.c)
  zephyr_library_sources_ifdef(CONFIG_POWER_STAGE_SIM power_stage_sim.c sim_plant.c)
  zephyr_library_sources_ifdef(CONFIG_SIM_ENCODER sim_encoder.c)
endif()
