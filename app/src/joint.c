/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit conversions between Stepper-C and the DYNAMIXEL X430 Control Table.
 */

#include "joint.h"

#include <x430_defs.h>

/* X430 units, as integer fractions:
 *   velocity:     0.229 rev/min      = 229 / 60000 rev/s
 *   acceleration: 214.577 rev/min^2  = 214577 / 3600000 rev/s^2
 *   current:      2.69 mA            = 269 / 100 mA
 */
#define VEL_NUM   229LL
#define VEL_DEN   60000LL
#define ACC_NUM   214577LL
#define ACC_DEN   3600000LL
#define CUR_NUM   269LL
#define CUR_DEN   100LL

/* Signed division rounded half away from zero; den > 0. */
static int64_t div_round(int64_t num, int64_t den)
{
	return (num >= 0) ? ((num + den / 2) / den) : -((-num + den / 2) / den);
}

static int32_t clamp_i32(int64_t v)
{
	if (v > INT32_MAX) {
		return INT32_MAX;
	}
	if (v < INT32_MIN) {
		return INT32_MIN;
	}
	return (int32_t)v;
}

static uint32_t clamp_u32(uint64_t v, uint32_t lo, uint32_t hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return (uint32_t)v;
}

/* Microsteps per joint revolution as the fraction usteps_num / usteps_den. */
static int64_t usteps_num(const struct joint_cfg *cfg)
{
	return (int64_t)cfg->full_steps_per_rev * cfg->microsteps * cfg->gear_motor_revs;
}

static int64_t usteps_den(const struct joint_cfg *cfg)
{
	return (int64_t)cfg->gear_joint_revs;
}

static int32_t direction(const struct joint_regs *regs)
{
	return ((regs->drive_mode & X430_DRIVE_MODE_REVERSE) != 0U) ? -1 : 1;
}

int32_t joint_position_to_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
			      int32_t microsteps)
{
	const int64_t pulses = div_round((int64_t)microsteps * JOINT_DXL_PULSES_PER_REV *
						 usteps_den(cfg),
					 usteps_num(cfg));

	return clamp_i32((int64_t)direction(regs) * pulses + cfg->start_position +
			 regs->homing_offset);
}

int32_t joint_position_from_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
				int32_t pulses)
{
	const int64_t rel = (int64_t)pulses - cfg->start_position - regs->homing_offset;
	const int64_t usteps = div_round(rel * usteps_num(cfg),
					 (int64_t)JOINT_DXL_PULSES_PER_REV * usteps_den(cfg));

	return clamp_i32((int64_t)direction(regs) * usteps);
}

int32_t joint_velocity_to_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
			      int32_t microsteps_per_s)
{
	const int64_t v = div_round((int64_t)microsteps_per_s * VEL_DEN * usteps_den(cfg),
				    VEL_NUM * usteps_num(cfg));

	return clamp_i32((int64_t)direction(regs) * v);
}

int32_t joint_velocity_from_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
				int32_t dxl_velocity)
{
	const int64_t v = div_round((int64_t)dxl_velocity * VEL_NUM * usteps_num(cfg),
				    VEL_DEN * usteps_den(cfg));

	return clamp_i32((int64_t)direction(regs) * v);
}

uint32_t joint_speed_from_dxl(const struct joint_cfg *cfg, uint32_t dxl_velocity)
{
	return clamp_u32((uint64_t)div_round((int64_t)dxl_velocity * VEL_NUM * usteps_num(cfg),
					     VEL_DEN * usteps_den(cfg)),
			 0U, UINT32_MAX);
}

uint32_t joint_accel_from_dxl(const struct joint_cfg *cfg, uint32_t dxl_accel)
{
	return clamp_u32((uint64_t)div_round((int64_t)dxl_accel * ACC_NUM * usteps_num(cfg),
					     ACC_DEN * usteps_den(cfg)),
			 0U, UINT32_MAX);
}

int32_t joint_current_to_dxl(int32_t milliamps)
{
	return clamp_i32(div_round((int64_t)milliamps * CUR_DEN, CUR_NUM));
}

uint32_t joint_current_from_dxl(uint32_t dxl_current)
{
	return clamp_u32((uint64_t)div_round((int64_t)dxl_current * CUR_NUM, CUR_DEN), 0U,
			 UINT32_MAX);
}

struct joint_profile joint_profile(const struct joint_cfg *cfg, const struct joint_regs *regs,
				   uint32_t distance, uint32_t profile_velocity,
				   uint32_t profile_acceleration, uint32_t velocity_limit,
				   uint32_t v_cap, uint32_t a_cap)
{
	uint64_t v = v_cap;
	uint64_t a = a_cap;

	if ((regs->drive_mode & X430_DRIVE_MODE_TIME_BASED) != 0U) {
		/* Time-based: t_total = Profile Velocity [ms], t_acc = Profile
		 * Acceleration [ms] (at most t_total / 2). A symmetric trapezoid
		 * covers distance = v * (t_total - t_acc).
		 */
		const uint32_t t_total = profile_velocity;
		uint32_t t_acc = profile_acceleration;

		if (t_acc > (t_total / 2U)) {
			t_acc = t_total / 2U;
		}
		if ((t_total > 0U) && (distance > 0U)) {
			v = ((uint64_t)distance * 1000U + (t_total - t_acc) - 1U) / (t_total - t_acc);
			if (t_acc > 0U) {
				a = (v * 1000U + t_acc - 1U) / t_acc;
			}
		}
	} else {
		if (profile_velocity > 0U) {
			v = joint_speed_from_dxl(cfg, profile_velocity);
		}
		if (profile_acceleration > 0U) {
			a = joint_accel_from_dxl(cfg, profile_acceleration);
		}
	}

	if (velocity_limit > 0U) {
		const uint32_t v_lim = joint_speed_from_dxl(cfg, velocity_limit);

		if (v > v_lim) {
			v = v_lim;
		}
	}

	return (struct joint_profile){
		.v_max = clamp_u32(v, 1U, v_cap),
		.accel = clamp_u32(a, 1U, a_cap),
	};
}
