# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_POWER_STAGE power_stage)
add_subdirectory_ifdef(CONFIG_SSTEP_MOTOR motor)
add_subdirectory_ifdef(CONFIG_X430_SERVO x430)
add_subdirectory_ifdef(CONFIG_MA730 ma730)
add_subdirectory_ifdef(CONFIG_WS2812_STM32_TIM ws2812)
