/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Doppelstep: 17HS19-2004S1 stepper motor on B-G431B-ESC1, controlled
 * by Stepper-C, seen by the host as a DYNAMIXEL XM430-W350 (Protocol 2.0).
 *
 * The drivers have initialised themselves, with the outputs off. main() measures
 * the current sensor offsets, starts the control task, connects the servo to the
 * motor and starts the communication task. Until then nothing can send a command, so
 * the motor cannot start with uncalibrated sensors. Then main() shows the state on the
 * STATUS LED.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <doppelstep/drivers/power_stage.h>

#include <doppelstep/drivers/x430_servo.h>

#include "app_msg.h"
#include "app_tasks.h"
#include "joint_bridge.h"

LOG_MODULE_REGISTER(main, CONFIG_SFS_LOG_LEVEL);

#define MOTOR_NODE DT_CHOSEN(sfs_motor)

static const struct device *const stage = DEVICE_DT_GET(DT_PHANDLE(MOTOR_NODE, power_stage));
static const struct device *const motor = DEVICE_DT_GET(MOTOR_NODE);
static const struct device *const motor_drv = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, driver));
static const struct device *const motor_ctrl = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, ctrl));
static const struct device *const servo = DEVICE_DT_GET(DT_CHOSEN(sfs_servo));
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* STATUS LED: slow blink = outputs off, on = outputs on, fast blink = fault
 * or startup error.
 */
static void status_led_loop(bool startup_failed)
{
	bool on = false;

	for (;;) {
		struct sstep_motor_status status;
		int32_t period_ms;

		if (startup_failed) {
			period_ms = 100;
		} else {
			status.state = SSTEP_MOTOR_DISABLED;
			(void)app_status_get(&status);
			switch (status.state) {
			case SSTEP_MOTOR_FAULT:
				period_ms = 100;
				break;
			case SSTEP_MOTOR_HOLDING:
			case SSTEP_MOTOR_MOVING:
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

static int startup(void)
{
	int ret;

	if (!device_is_ready(stage) || !device_is_ready(motor) || !device_is_ready(motor_drv) ||
	    !device_is_ready(motor_ctrl) || !device_is_ready(servo)) {
		return -ENODEV;
	}

	ret = power_stage_calibrate(stage, CONFIG_SFS_STAGE_OFFSET_SAMPLES);
	if (ret != 0) {
		return ret;
	}

	/* Runs at once: higher priority, and it publishes the first status */
	k_thread_start(control_task_id);

	ret = joint_bridge_init(servo);
	if (ret != 0) {
		return ret;
	}

	k_thread_start(comm_task_id);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("Doppelstep on %s", CONFIG_BOARD_TARGET);

	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	ret = startup();
	if (ret != 0) {
		LOG_ERR("startup failed: %d", ret);
	}

	status_led_loop(ret != 0);
	return 0;
}
