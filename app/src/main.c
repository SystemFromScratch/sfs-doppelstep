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

#include <errno.h>

#include <zephyr/app_version.h>
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
#include "supervisor.h"

LOG_MODULE_REGISTER(main, CONFIG_SFS_LOG_LEVEL);

#define MOTOR_NODE DT_CHOSEN(sfs_motor)

static const struct device *const stage = DEVICE_DT_GET(DT_PHANDLE(MOTOR_NODE, power_stage));
static const struct device *const motor = DEVICE_DT_GET(MOTOR_NODE);
static const struct device *const motor_drv = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, driver));
static const struct device *const motor_ctrl = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, ctrl));
static const struct device *const servo = DEVICE_DT_GET(DT_CHOSEN(sfs_servo));
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* STATUS LED: slow blink = outputs off, on = outputs on, fast blink = fault
 * or startup error. The same loop checks the tasks every CONFIG_SFS_WATCHDOG_CHECK_MS
 * and feeds the watchdog (supervisor.h).
 */
static void status_led_loop(bool startup_failed)
{
	bool on = false;
	int32_t since_toggle_ms = 0;

	for (;;) {
		struct sstep_motor_status status;
		int32_t period_ms;

		k_msleep(CONFIG_SFS_WATCHDOG_CHECK_MS);
		if (startup_failed) {
			/* The comm task may never have started, so no command can come
			 * and the outputs stay off: keep the fast blink instead of a
			 * reset loop, even if the watchdog already runs.
			 */
			supervisor_checkin(SUPERVISOR_CONTROL);
			supervisor_checkin(SUPERVISOR_COMM);
		}
		(void)supervisor_poll();
		since_toggle_ms += CONFIG_SFS_WATCHDOG_CHECK_MS;

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

		if ((period_ms == 0) || (since_toggle_ms >= period_ms)) {
			since_toggle_ms = 0;
			on = (period_ms == 0) ? true : !on;
			(void)gpio_pin_set_dt(&led, on ? 1 : 0);
		}
	}
}

static int startup(void)
{
	int ret;

	if (!device_is_ready(stage) || !device_is_ready(motor) || !device_is_ready(motor_drv) ||
	    !device_is_ready(motor_ctrl) || !device_is_ready(servo)) {
		return -ENODEV;
	}

	/* -EIO: an offset outside its window (broken amplifier, shunt or bias). The
	 * stage refuses its outputs; the bus still runs and reports Electrical Shock.
	 */
	ret = power_stage_calibrate(stage, CONFIG_SFS_STAGE_OFFSET_SAMPLES);
	const bool sensor_fault = (ret == -EIO);

	if ((ret != 0) && !sensor_fault) {
		return ret;
	}

	/* Without the watchdog the tasks do not start, so the outputs stay off. The
	 * first check comes CONFIG_SFS_WATCHDOG_CHECK_MS later, with both tasks running.
	 */
	ret = supervisor_start();
	if (ret != 0) {
		return ret;
	}

	/* Runs at once: higher priority, and it publishes the first status */
	k_thread_start(control_task_id);

	ret = joint_bridge_init(servo);
	if (ret != 0) {
		return ret;
	}
	if (sensor_fault) {
		x430_servo_lock(servo);
		x430_servo_raise_hw_error(servo, X430_HW_ERROR_ELECTRICAL_SHOCK);
		x430_servo_unlock(servo);
	}

	k_thread_start(comm_task_id);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("Doppelstep %s on %s", APP_VERSION_STRING, CONFIG_BOARD_TARGET);

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
