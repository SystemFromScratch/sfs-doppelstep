/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit conversions between the stepper motor (Stepper-C: microsteps,
 * microsteps/s, microsteps/s^2, mA) and the DYNAMIXEL X430 Control Table
 * (pulse = 360/4096 deg, 0.229 rev/min, 214.577 rev/min^2, 2.69 mA, ms).
 * Pure functions, no state: unit-testable on the host.
 */

#ifndef SFS_JOINT_H
#define SFS_JOINT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** X430 position resolution [pulse per joint revolution]. */
#define JOINT_DXL_PULSES_PER_REV 4096

/** Mechanical and register-level parameters of one joint. */
struct joint_cfg {
	uint32_t full_steps_per_rev; /**< Motor full steps per motor revolution. */
	uint32_t microsteps;         /**< Microsteps per full step. */
	uint32_t gear_motor_revs;    /**< Motor revolutions ...          */
	uint32_t gear_joint_revs;    /**< ... per joint revolutions.     */
	int32_t start_position;      /**< Present Position of motor position 0 [pulse]. */
};

/** Register values that change the mapping (cached from the Control Table). */
struct joint_regs {
	uint8_t drive_mode;     /**< Drive Mode (10). */
	int32_t homing_offset;  /**< Homing Offset (20) [pulse]. */
};

/** Motor position [microsteps] to Present Position [pulse]. */
int32_t joint_position_to_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
			      int32_t microsteps);

/** Goal Position [pulse] to motor position [microsteps]. */
int32_t joint_position_from_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
				int32_t pulses);

/** Motor velocity [microsteps/s] to Present Velocity [0.229 rev/min]. */
int32_t joint_velocity_to_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
			      int32_t microsteps_per_s);

/** Goal Velocity [0.229 rev/min] to motor velocity [microsteps/s]. */
int32_t joint_velocity_from_dxl(const struct joint_cfg *cfg, const struct joint_regs *regs,
				int32_t dxl_velocity);

/** Velocity magnitude [0.229 rev/min] (Profile Velocity, Velocity Limit) to [microsteps/s]. */
uint32_t joint_speed_from_dxl(const struct joint_cfg *cfg, uint32_t dxl_velocity);

/** Profile Acceleration [214.577 rev/min^2] to [microsteps/s^2]. */
uint32_t joint_accel_from_dxl(const struct joint_cfg *cfg, uint32_t dxl_accel);

/** Phase current amplitude [mA] to Present Current [2.69 mA]. */
int32_t joint_current_to_dxl(int32_t milliamps);

/** Current Limit [2.69 mA] to [mA]. */
uint32_t joint_current_from_dxl(uint32_t dxl_current);

/** Motion limits of one move. */
struct joint_profile {
	uint32_t v_max; /**< [microsteps/s], >= 1 */
	uint32_t accel; /**< [microsteps/s^2], >= 1 */
};

/**
 * @brief Velocity and acceleration of a position move from the profile
 *        registers, as an X430 interprets them.
 *
 * Velocity-based profile (Drive Mode bit 2 = 0): Profile Velocity in
 * 0.229 rev/min and Profile Acceleration in 214.577 rev/min^2; 0 = no limit.
 * Time-based profile (bit 2 = 1): Profile Velocity is the time of the whole
 * move and Profile Acceleration the acceleration time, both in ms; the
 * acceleration time is limited to half of the move time; 0 = no limit.
 * The result is limited by @p v_cap / @p a_cap and by Velocity Limit (> 0).
 *
 * @param distance        |target - position| [microsteps].
 * @param velocity_limit  Velocity Limit (44) [0.229 rev/min], 0 = none.
 */
struct joint_profile joint_profile(const struct joint_cfg *cfg, const struct joint_regs *regs,
				   uint32_t distance, uint32_t profile_velocity,
				   uint32_t profile_acceleration, uint32_t velocity_limit,
				   uint32_t v_cap, uint32_t a_cap);

#ifdef __cplusplus
}
#endif

#endif /* SFS_JOINT_H */
