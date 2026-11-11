/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See joint_bridge.h. The servo callbacks run in the communication task with the
 * bus lock held; a command waits for its result, because the status packet of the
 * host carries it.
 */

#include "joint_bridge.h"

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <doppelstep/drivers/x430_servo.h>

#include "app_msg.h"
#include "joint.h"

#define SERVO_NODE DT_CHOSEN(sfs_servo)
#define MOTOR_NODE DT_CHOSEN(sfs_motor)

#define V_MAX     DT_PROP(MOTOR_NODE, max_velocity)
#define ACCEL_MAX DT_PROP(MOTOR_NODE, max_acceleration)
#define I_HOLD_MA DT_PROP(MOTOR_NODE, hold_current_milliamp)
#define I_RUN_MA  DT_PROP(MOTOR_NODE, run_current_milliamp)

/* Largest Velocity Limit of the X430 model (models/x430_family.yaml of the emulator) */
#define DXL_VELOCITY_LIMIT_MAX 1023

static const struct device *servo;

static const struct joint_cfg jcfg = {
	.full_steps_per_rev = DT_PROP(MOTOR_NODE, full_steps_per_rev),
	.microsteps = DT_PROP(MOTOR_NODE, microsteps),
	.gear_motor_revs = DT_PROP(SERVO_NODE, gear_motor_revs),
	.gear_joint_revs = DT_PROP(SERVO_NODE, gear_joint_revs),
	.start_position = DT_PROP(SERVO_NODE, start_position),
};

/* Control Table values the mapping needs, kept current by the write hook. */
static struct {
	struct joint_regs regs;
	uint8_t operating_mode;
	uint32_t profile_velocity;
	uint32_t profile_acceleration;
	uint32_t velocity_limit;
} cache;

static uint16_t faults_reported;

/* Latest status; kept if a reader finds the queue empty during a publication */
static struct sstep_motor_status status;

static const struct sstep_motor_status *latest_status(void)
{
	(void)app_status_get(&status);
	return &status;
}

/* ------------------------------------------------------------------------ */
/* Control Table <-> motor                                                  */

static int32_t reg(uint16_t address)
{
	return x430_servo_reg_get(servo, address);
}

static void reload_cache(void)
{
	cache.regs.drive_mode = (uint8_t)reg(X430_ADDR_DRIVE_MODE);
	cache.regs.homing_offset = reg(X430_ADDR_HOMING_OFFSET);
	cache.operating_mode = (uint8_t)reg(X430_ADDR_OPERATING_MODE);
	cache.profile_velocity = (uint32_t)reg(X430_ADDR_PROFILE_VELOCITY);
	cache.profile_acceleration = (uint32_t)reg(X430_ADDR_PROFILE_ACCELERATION);
	cache.velocity_limit = (uint32_t)reg(X430_ADDR_VELOCITY_LIMIT);
}

static uint8_t hardware_error_bits(uint16_t faults)
{
	uint8_t bits = 0U;

	if ((faults & (SSTEP_MOTOR_FAULT_OVERVOLTAGE | SSTEP_MOTOR_FAULT_UNDERVOLTAGE)) != 0U) {
		bits |= X430_HW_ERROR_INPUT_VOLTAGE;
	}
	if ((faults & (SSTEP_MOTOR_FAULT_OVERCURRENT | SSTEP_MOTOR_FAULT_EXTERNAL |
		       SSTEP_MOTOR_FAULT_HAL)) != 0U) {
		bits |= X430_HW_ERROR_ELECTRICAL_SHOCK;
	}
	return bits;
}

/* Live registers from the motor snapshot. Bus lock held. */
static void refresh_live(void)
{
	const struct sstep_motor_status *st = latest_status();
	const int32_t position = joint_position_to_dxl(&jcfg, &cache.regs, st->position);
	const int32_t velocity = joint_velocity_to_dxl(&jcfg, &cache.regs, st->velocity);
	const bool moving = st->state == SSTEP_MOTOR_MOVING;
	uint8_t moving_status = 0U;

	(void)x430_servo_reg_set(servo, X430_ADDR_PRESENT_POSITION, position);
	(void)x430_servo_reg_set(servo, X430_ADDR_POSITION_TRAJECTORY, position);
	(void)x430_servo_reg_set(servo, X430_ADDR_PRESENT_VELOCITY, velocity);
	(void)x430_servo_reg_set(servo, X430_ADDR_VELOCITY_TRAJECTORY, velocity);
	(void)x430_servo_reg_set(servo, X430_ADDR_PRESENT_CURRENT,
			      joint_current_to_dxl((int32_t)st->i_amplitude_ma));
	(void)x430_servo_reg_set(servo, X430_ADDR_PRESENT_INPUT_VOLTAGE,
			      (int32_t)((st->vbus_mv + 50U) / 100U));
	(void)x430_servo_reg_set(servo, X430_ADDR_MOVING, moving ? 1 : 0);

	if (moving) {
		moving_status |= X430_MOVING_STATUS_PROFILE_ONGOING |
				 X430_MOVING_STATUS_PROFILE_TRAPEZOIDAL;
	} else if (st->position == st->target) {
		moving_status |= X430_MOVING_STATUS_IN_POSITION;
	}
	(void)x430_servo_reg_set(servo, X430_ADDR_MOVING_STATUS, moving_status);

	/* Faults of the power stage -> Hardware Error Status (once per new fault) */
	const uint16_t fresh = st->faults & (uint16_t)~faults_reported;

	if (fresh != 0U) {
		const uint8_t bits = hardware_error_bits(fresh);

		if (bits != 0U) {
			x430_servo_raise_hw_error(servo, bits);
		}
	}
	faults_reported = st->faults;

	/* Outputs off while Torque Enable says on (fault, external disable) */
	if ((reg(X430_ADDR_TORQUE_ENABLE) != 0) &&
	    ((st->state == SSTEP_MOTOR_DISABLED) || (st->state == SSTEP_MOTOR_FAULT))) {
		(void)x430_servo_reg_set(servo, X430_ADDR_TORQUE_ENABLE, 0);
	}
}

static uint8_t to_status(int rc)
{
	return (rc == 0) ? DXL2_ERR_NONE : DXL2_ERR_RESULT_FAIL;
}

static uint8_t goal_position(int32_t value)
{
	struct joint_profile profile;
	int32_t target;

	if (((cache.operating_mode != X430_MODE_POSITION) &&
	     (cache.operating_mode != X430_MODE_EXTENDED_POSITION)) ||
	    (reg(X430_ADDR_TORQUE_ENABLE) == 0)) {
		return DXL2_ERR_NONE; /* stored only, as on an X430 */
	}

	target = joint_position_from_dxl(&jcfg, &cache.regs, value);
	profile = joint_profile(&jcfg, &cache.regs,
				(uint32_t)llabs((int64_t)target - latest_status()->position),
				cache.profile_velocity, cache.profile_acceleration,
				cache.velocity_limit, V_MAX, ACCEL_MAX);

	return to_status(app_call(APP_CMD_MOVE_TO, target, profile.v_max, profile.accel));
}

static uint8_t goal_velocity(int32_t value)
{
	uint32_t cap = V_MAX;
	uint32_t accel = ACCEL_MAX;
	int32_t velocity;

	if ((cache.operating_mode != X430_MODE_VELOCITY) || (reg(X430_ADDR_TORQUE_ENABLE) == 0)) {
		return DXL2_ERR_NONE;
	}

	if (cache.velocity_limit > 0U) {
		cap = MIN(cap, joint_speed_from_dxl(&jcfg, cache.velocity_limit));
	}
	if (cache.profile_acceleration > 0U) {
		accel = CLAMP(joint_accel_from_dxl(&jcfg, cache.profile_acceleration), 1U,
			      (uint32_t)ACCEL_MAX);
	}
	velocity = CLAMP(joint_velocity_from_dxl(&jcfg, &cache.regs, value), -(int32_t)cap,
			 (int32_t)cap);

	return to_status(app_call(APP_CMD_RUN, velocity, MAX((uint32_t)abs(velocity), 1U), accel));
}

/* ------------------------------------------------------------------------ */
/* Servo callbacks (bus thread, bus lock held)                              */

static uint8_t on_write(const struct device *dev, uint16_t address, uint8_t size, int32_t value,
			void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(size);
	ARG_UNUSED(user_data);

	switch (address) {
	case X430_ADDR_TORQUE_ENABLE:
		if (value != 0) {
			/* The library copies Present Position into Goal Position next */
			refresh_live();
			return to_status(app_call(APP_CMD_ENABLE, 0, 0U, 0U));
		}
		return to_status(app_call(APP_CMD_DISABLE, 0, 0U, 0U));

	case X430_ADDR_OPERATING_MODE:
		/* Position, extended position and velocity map onto the motor;
		 * current, current-based position and PWM need a torque path.
		 */
		if ((value != X430_MODE_POSITION) && (value != X430_MODE_EXTENDED_POSITION) &&
		    (value != X430_MODE_VELOCITY)) {
			return DXL2_ERR_DATA_RANGE;
		}
		cache.operating_mode = (uint8_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_DRIVE_MODE:
		cache.regs.drive_mode = (uint8_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_HOMING_OFFSET:
		cache.regs.homing_offset = value;
		return DXL2_ERR_NONE;

	case X430_ADDR_PROFILE_VELOCITY:
		cache.profile_velocity = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_PROFILE_ACCELERATION:
		cache.profile_acceleration = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_VELOCITY_LIMIT:
		cache.velocity_limit = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_CURRENT_LIMIT:
		return to_status(app_call(APP_CMD_SET_CURRENT, 0,
					    joint_current_from_dxl((uint32_t)value), I_HOLD_MA));

	case X430_ADDR_GOAL_POSITION:
		return goal_position(value);

	case X430_ADDR_GOAL_VELOCITY:
		return goal_velocity(value);

	default:
		return DXL2_ERR_NONE;
	}
}

static void on_read(const struct device *dev, uint16_t address, uint16_t length, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if ((address + length) > X430_ADDR_REALTIME_TICK) {
		refresh_live();
	}
}

static uint8_t on_event(const struct device *dev, enum x430_event event, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (event) {
	case X430_EVENT_REBOOT:
	case X430_EVENT_FACTORY_RESET:
		(void)app_call(APP_CMD_DISABLE, 0, 0U, 0U);
		(void)app_call(APP_CMD_CLEAR_FAULTS, 0, 0U, 0U);
		faults_reported = 0U;
		reload_cache();
		break;

	case X430_EVENT_CLEAR_MULTI_TURN: {
		const int32_t present = joint_position_to_dxl(&jcfg, &cache.regs,
							      latest_status()->position);
		const int32_t wrapped = ((present % JOINT_DXL_PULSES_PER_REV) +
					 JOINT_DXL_PULSES_PER_REV) %
					JOINT_DXL_PULSES_PER_REV;

		return to_status(app_call(APP_CMD_SET_POSITION,
					    joint_position_from_dxl(&jcfg, &cache.regs, wrapped),
					    0U, 0U));
	}

	case X430_EVENT_BUS_WATCHDOG:
		(void)app_call(APP_CMD_STOP, 0, 0U, 0U);
		break;

	case X430_EVENT_TORQUE_SHUTDOWN:
		(void)app_call(APP_CMD_DISABLE, 0, 0U, 0U);
		break;

	case X430_EVENT_BACKUP_RESTORED:
		reload_cache();
		(void)app_call(APP_CMD_SET_CURRENT, 0,
			       joint_current_from_dxl((uint32_t)reg(X430_ADDR_CURRENT_LIMIT)),
			       I_HOLD_MA);
		break;

	default:
		break;
	}
	return DXL2_ERR_NONE;
}

static bool on_nvm_allowed(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	/* A flash erase stalls the CPU (single bank) and with it the current loop */
	return latest_status()->state == SSTEP_MOTOR_DISABLED;
}

static const struct x430_servo_callbacks callbacks = {
	.write = on_write,
	.read = on_read,
	.event = on_event,
	.nvm_allowed = on_nvm_allowed,
};

/* ------------------------------------------------------------------------ */

/* Factory values of the real motor in place of those of an XM430 (docs/dynamixel.md):
 * Current Limit = run current, Velocity Limit = largest motor velocity at the joint,
 * both rounded down so that the register never shows more than the motor gets.
 */
static void set_factory_values(void)
{
	static const struct joint_regs normal = {0};
	int32_t current = joint_current_to_dxl(I_RUN_MA);
	int32_t velocity = joint_velocity_to_dxl(&jcfg, &normal, V_MAX);

	if (joint_current_from_dxl((uint32_t)current) > I_RUN_MA) {
		current--;
	}
	if (joint_speed_from_dxl(&jcfg, (uint32_t)velocity) > V_MAX) {
		velocity--;
	}
	velocity = MIN(velocity, DXL_VELOCITY_LIMIT_MAX);
	(void)x430_servo_set_factory_value(servo, X430_ADDR_CURRENT_LIMIT, 2U, current);
	(void)x430_servo_set_factory_value(servo, X430_ADDR_VELOCITY_LIMIT, 4U, velocity);
}

int joint_bridge_init(const struct device *dev)
{
	servo = dev;
	set_factory_values();

	x430_servo_lock(servo);
	reload_cache();
	refresh_live();
	x430_servo_unlock(servo);

	return x430_servo_start(servo, &callbacks, NULL);
}

void joint_bridge_refresh(void)
{
	x430_servo_lock(servo);
	refresh_live();
	x430_servo_unlock(servo);
}
