/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stepper-C driver instance: configuration from Kconfig and the power stage,
 * fast loop in the power-stage interrupt, slow loop in a cooperative thread.
 * Every Stepper-C command runs in the slow-loop thread; other threads call the
 * motor_*() functions below, which hand the command over and wait for it.
 */

#ifndef SFS_MOTOR_H
#define SFS_MOTOR_H

#include <stdbool.h>
#include <stdint.h>

#include <sstep.h>

#ifdef __cplusplus
extern "C" {
#endif

/** State published by the slow loop every period. */
struct motor_snapshot {
	struct sstep_status status; /**< sstep_get_status() of the last period. */
	uint32_t i_amplitude_ma;    /**< sqrt(iA^2 + iB^2) [mA]. */
	uint32_t i_run_ma;          /**< Active run current [mA]. */
	uint32_t i_hold_ma;         /**< Active hold current [mA]. */
	uint32_t sequence;          /**< Incremented every slow-loop period. */
};

/** Initialise Stepper-C and start the slow loop (power stage must be initialised). */
int motor_init(void);

/** Copy of the latest snapshot (any thread). */
void motor_get_snapshot(struct motor_snapshot *snapshot);

/** Enable (hold position) or disable (coast) the outputs. */
int motor_enable(bool enable);

/** Move to @p target [microsteps] with the given limits. */
int motor_move_to(int32_t target, uint32_t v_max, uint32_t accel);

/** Run at @p velocity [microsteps/s] with acceleration @p accel. */
int motor_run(int32_t velocity, uint32_t v_max, uint32_t accel);

/** Decelerate to standstill. */
int motor_stop(void);

/** Redefine the current position [microsteps] (standstill only). */
int motor_set_position(int32_t position);

/** Change run and hold currents [mA]; limited to the configured values. */
int motor_set_current(uint32_t run_ma, uint32_t hold_ma);

/** Clear the faults (driver goes to disabled). */
int motor_clear_faults(void);

/** true when the gate drivers may be enabled (dead time configured). */
bool motor_outputs_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* SFS_MOTOR_H */
