/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit conversions between the motor and the X430 Control Table. Expected values
 * from the X430 units: 4096 pulse/rev, 0.229 rev/min, 214.577 rev/min^2, 2.69 mA.
 * The motor: 200 full steps * 16 microsteps = 3200 microsteps/rev.
 */

#include <zephyr/ztest.h>

#include <x430_defs.h>

#include "joint.h"

#define V_CAP 16000U
#define A_CAP 64000U

static const struct joint_cfg cfg = {
	.full_steps_per_rev = 200U,
	.microsteps = 16U,
	.gear_motor_revs = 1U,
	.gear_joint_revs = 1U,
	.start_position = 2048,
};

static const struct joint_regs regs = {0};
static const struct joint_regs reversed = {.drive_mode = X430_DRIVE_MODE_REVERSE};
static const struct joint_regs time_based = {.drive_mode = X430_DRIVE_MODE_TIME_BASED};

ZTEST(joint, test_position)
{
	zassert_equal(joint_position_to_dxl(&cfg, &regs, 3200), 6144);
	zassert_equal(joint_position_from_dxl(&cfg, &regs, 6144), 3200);
	zassert_equal(joint_position_to_dxl(&cfg, &regs, 0), 2048);
}

ZTEST(joint, test_reverse_mode_turns_the_direction)
{
	zassert_equal(joint_position_to_dxl(&cfg, &reversed, 800), 1024);
	zassert_equal(joint_position_from_dxl(&cfg, &reversed, 1024), 800);
	zassert_equal(joint_velocity_from_dxl(&cfg, &reversed, 100), -1221);
	zassert_equal(joint_velocity_to_dxl(&cfg, &reversed, 1221), -100);
}

ZTEST(joint, test_homing_offset)
{
	const struct joint_regs homed = {.homing_offset = 100};

	zassert_equal(joint_position_to_dxl(&cfg, &homed, 0), 2148);
	zassert_equal(joint_position_from_dxl(&cfg, &homed, 2148), 0);
}

ZTEST(joint, test_velocity)
{
	zassert_equal(joint_speed_from_dxl(&cfg, 1U), 12U);
	zassert_equal(joint_velocity_from_dxl(&cfg, &regs, -1), -12);
	zassert_equal(joint_velocity_to_dxl(&cfg, &regs, 12), 1);
}

ZTEST(joint, test_acceleration)
{
	zassert_equal(joint_accel_from_dxl(&cfg, 1U), 191U);
	zassert_equal(joint_accel_from_dxl(&cfg, 10U), 1907U);
}

ZTEST(joint, test_current)
{
	zassert_equal(joint_current_to_dxl(2690), 1000);
	zassert_equal(joint_current_from_dxl(1U), 3U);
}

ZTEST(joint, test_gear)
{
	const struct joint_cfg geared = {
		.full_steps_per_rev = 200U,
		.microsteps = 16U,
		.gear_motor_revs = 2U,
		.gear_joint_revs = 1U,
		.start_position = 2048,
	};

	zassert_equal(joint_position_to_dxl(&geared, &regs, 6400), 6144);
	zassert_equal(joint_position_from_dxl(&geared, &regs, 6144), 6400);
}

ZTEST(joint, test_time_based_profile)
{
	const struct joint_profile p =
		joint_profile(&cfg, &time_based, 3200U, 1000U, 200U, 0U, V_CAP, A_CAP);

	zassert_equal(p.v_max, 4000U);
	zassert_equal(p.accel, 20000U);
}

ZTEST(joint, test_time_based_acceleration_is_at_most_half_of_the_move)
{
	const struct joint_profile p =
		joint_profile(&cfg, &time_based, 3200U, 1000U, 800U, 0U, V_CAP, A_CAP);

	zassert_equal(p.v_max, 6400U);
	zassert_equal(p.accel, 12800U);
}

ZTEST(joint, test_velocity_based_profile)
{
	const struct joint_profile p =
		joint_profile(&cfg, &regs, 3200U, 100U, 10U, 0U, V_CAP, A_CAP);

	zassert_equal(p.v_max, 1221U);
	zassert_equal(p.accel, 1907U);
}

ZTEST(joint, test_zero_profile_means_the_caps)
{
	const struct joint_profile p = joint_profile(&cfg, &regs, 3200U, 0U, 0U, 0U, V_CAP, A_CAP);

	zassert_equal(p.v_max, V_CAP);
	zassert_equal(p.accel, A_CAP);
}

ZTEST(joint, test_profile_is_limited_by_the_caps)
{
	const struct joint_profile p =
		joint_profile(&cfg, &regs, 3200U, 2000U, 2000U, 0U, V_CAP, A_CAP);

	zassert_equal(p.v_max, V_CAP);
	zassert_equal(p.accel, A_CAP);
}

ZTEST(joint, test_velocity_limit_below_the_cap_wins)
{
	const struct joint_profile p = joint_profile(&cfg, &regs, 3200U, 0U, 0U, 50U, V_CAP, A_CAP);

	zassert_equal(p.v_max, 611U);
}

ZTEST_SUITE(joint, NULL, NULL, NULL, NULL, NULL);
