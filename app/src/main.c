/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Doppelstep: 17HS19-2004S1 stepper motor on the B-G431B-ESC1 or the full-bridge board,
 * controlled by Stepper-C, seen by the host as a DYNAMIXEL XM430-W350 (Protocol 2.0).
 *
 * The drivers have initialised themselves, with the outputs off. main() measures
 * the current sensor offsets, starts the control task, connects the servo to the
 * motor and starts the communication task. Until then nothing can send a command, so
 * the motor cannot start with uncalibrated sensors. Then main() shows the state on the
 * STATUS LED.
 */

#include <errno.h>

#include <doppelstep_version.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <doppelstep/drivers/power_stage.h>

#include <doppelstep/drivers/x430_servo.h>

#include "app_msg.h"
#include "app_tasks.h"
#include "joint_bridge.h"
#include "status_led.h"
#include "supervisor.h"

/* The startup check of the field flags is specific to the MA730 */
#define ENCODER_MA730                                                                              \
	COND_CODE_1(DT_HAS_CHOSEN(sfs_encoder),                                                    \
		    (DT_NODE_HAS_COMPAT(DT_CHOSEN(sfs_encoder), sfs_ma730)), (0))

#if ENCODER_MA730
#include <doppelstep/drivers/ma730.h>
#endif

LOG_MODULE_REGISTER(main, CONFIG_SFS_LOG_LEVEL);

#define MOTOR_NODE DT_CHOSEN(sfs_motor)

static const struct device *const stage = DEVICE_DT_GET(DT_PHANDLE(MOTOR_NODE, power_stage));
static const struct device *const motor = DEVICE_DT_GET(MOTOR_NODE);
static const struct device *const motor_drv = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, driver));
static const struct device *const motor_ctrl = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, ctrl));
static const struct device *const servo = DEVICE_DT_GET(DT_CHOSEN(sfs_servo));

/* STATUS LED, as the LED of an XM430: the LED register (65) switches it on and off,
 * and an alarm makes it blink fast whatever the register says: a fault of the motor,
 * a bit in Hardware Error Status (70) or a startup error. The same loop checks the
 * tasks every CONFIG_SFS_WATCHDOG_CHECK_MS and feeds the watchdog (supervisor.h).
 */
#define LED_ALARM_PERIOD_MS 100

static bool led_alarm(void)
{
	struct app_status status;
	int32_t hw_error;

	status.motor.state = SSTEP_MOTOR_DISABLED;
	(void)app_status_get(&status);

	x430_servo_lock(servo);
	hw_error = x430_servo_reg_get(servo, X430_ADDR_HARDWARE_ERROR_STATUS);
	x430_servo_unlock(servo);

	return (status.motor.state == SSTEP_MOTOR_FAULT) || (hw_error != 0);
}

static bool led_register(void)
{
	int32_t value;

	x430_servo_lock(servo);
	value = x430_servo_reg_get(servo, X430_ADDR_LED);
	x430_servo_unlock(servo);

	return value != 0;
}

static void status_led_loop(bool startup_failed)
{
	bool on = false;
	int32_t since_toggle_ms = 0;

	for (;;) {
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

		/* Without a successful startup the servo may not be connected:
		 * its registers are not read.
		 */
		if (startup_failed || led_alarm()) {
			if (since_toggle_ms >= LED_ALARM_PERIOD_MS) {
				since_toggle_ms = 0;
				on = !on;
			}
		} else {
			since_toggle_ms = 0;
			on = led_register();
		}
		status_led_set(on);
	}
}

#if ENCODER_MA730
/* Encoder check at startup: it answers, and the magnet is in range. Not fatal: the
 * motor runs open loop without it.
 */
static void encoder_check(void)
{
	const struct device *const enc = DEVICE_DT_GET(DT_CHOSEN(sfs_encoder));
	bool high = false;
	bool low = false;
	uint16_t angle = 0U;

	if (!device_is_ready(enc) || (ma730_read_field(enc, &high, &low) != 0) ||
	    (ma730_read_angle(enc, &angle) != 0)) {
		LOG_WRN("encoder %s does not answer", enc->name);
		return;
	}
	if (high || low) {
		LOG_WRN("encoder: magnetic field %s the threshold, angle %u/65536",
			high ? "above" : "below", angle);
	} else {
		LOG_INF("encoder: angle %u/65536, field in range", angle);
	}
}
#endif

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

#if ENCODER_MA730
	encoder_check();
#endif

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

	LOG_INF("Doppelstep %s on %s", DOPPELSTEP_VERSION_STRING, CONFIG_BOARD_TARGET);

	status_led_init();

	ret = startup();
	if (ret != 0) {
		LOG_ERR("startup failed: %d", ret);
	}

	status_led_loop(ret != 0);
	return 0;
}
