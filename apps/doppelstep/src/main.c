/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Doppelstep: 17HS19-2004S1 stepper motor on B-G431B-ESC1, controlled
 * by Stepper-C, seen by the host as a DYNAMIXEL XM430-W350 (Protocol 2.0).
 *
 * Startup:
 *   1. power stage: OPAMPs, TIM1 (outputs off), ADC1/ADC2, fast-loop interrupt
 *   2. current sensor offsets, measured with the outputs off
 *   3. Stepper-C (sstep_init) and the slow-loop thread
 *   4. DYNAMIXEL bus (NVM, X430 device, UART) and its thread
 * Then main() shows the state on the STATUS LED.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dxl_bus.h"
#include "motor.h"
#include "power_stage.h"

LOG_MODULE_REGISTER(main, CONFIG_SFS_LOG_LEVEL);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* STATUS LED: slow blink = outputs off, on = outputs on, fast blink = fault
 * or startup error.
 */
static void status_led_loop(bool startup_failed)
{
	bool on = false;

	for (;;) {
		struct motor_snapshot snap;
		int32_t period_ms;

		if (startup_failed) {
			period_ms = 100;
		} else {
			motor_get_snapshot(&snap);
			switch (snap.status.state) {
			case SSTEP_STATE_FAULT:
				period_ms = 100;
				break;
			case SSTEP_STATE_HOLDING:
			case SSTEP_STATE_MOVING:
				period_ms = 0;
				break;
			default:
				period_ms = 500;
				break;
			}
		}

		on = (period_ms == 0) ? true : !on;
		(void)gpio_pin_set_dt(&led, on ? 1 : 0);
		k_msleep((period_ms == 0) ? 100 : period_ms);
	}
}

int main(void)
{
	int ret;

	LOG_INF("Doppelstep on %s", CONFIG_BOARD_TARGET);

	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	ret = power_stage_init();
	if (ret == 0) {
		ret = power_stage_calibrate(CONFIG_SFS_STAGE_OFFSET_SAMPLES);
	}
	if (ret == 0) {
		ret = motor_init();
	}
	if (ret == 0) {
		ret = dxl_bus_init();
	}
	if (ret != 0) {
		LOG_ERR("startup failed: %d", ret);
	}

	status_led_loop(ret != 0);
	return 0;
}
