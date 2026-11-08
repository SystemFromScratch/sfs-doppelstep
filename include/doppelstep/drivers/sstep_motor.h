/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stepper motor driver built on Stepper-C. Like an integrated chip in the Zephyr
 * tree (adi,tmc50xx) it has a parent node and two children: the child
 * sfs,sstep-stepper-driver provides the standard class "stepper" (enable, disable,
 * microsteps) and the child sfs,sstep-stepper-ctrl the standard class "stepper_ctrl"
 * (moves, ramp, position). This header is the extension on the parent node, for what
 * the standard classes do not have.
 *
 * Thread contract: the driver has no thread and no lock. Every function, of the
 * standard classes too, is called from one thread: the one that calls
 * sstep_motor_process() at the rate of the node's slow-loop-hz property.
 *
 * Fast loop: in the interrupt of the power stage. A fault switches the outputs off
 * there at once, whatever the threads are doing; the event follows from
 * sstep_motor_process().
 */

#ifndef DOPPELSTEP_DRIVERS_SSTEP_MOTOR_H_
#define DOPPELSTEP_DRIVERS_SSTEP_MOTOR_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sstep_motor_state {
	SSTEP_MOTOR_DISABLED, /**< Outputs off. */
	SSTEP_MOTOR_HOLDING,  /**< Outputs on, position held. */
	SSTEP_MOTOR_MOVING,   /**< A move or a run is executing. */
	SSTEP_MOTOR_FAULT,    /**< Outputs off after a fault, see sstep_motor_clear_faults(). */
};

#define SSTEP_MOTOR_FAULT_OVERCURRENT  BIT(0)
#define SSTEP_MOTOR_FAULT_OVERVOLTAGE  BIT(1)
#define SSTEP_MOTOR_FAULT_UNDERVOLTAGE BIT(2)
#define SSTEP_MOTOR_FAULT_HAL          BIT(3) /**< The power stage refused a command. */
#define SSTEP_MOTOR_FAULT_EXTERNAL     BIT(4) /**< The power stage reported its aux trip. */
#define SSTEP_MOTOR_FAULT_STALL        BIT(5)

struct sstep_motor_status {
	enum sstep_motor_state state;
	uint16_t faults;         /**< Active SSTEP_MOTOR_FAULT_* flags. */
	int32_t position;        /**< Commanded position [microsteps]. */
	int32_t target;          /**< Target of the current move [microsteps]. */
	int32_t velocity;        /**< Commanded velocity [microsteps/s]. */
	uint32_t i_amplitude_ma; /**< Root of iA^2 + iB^2 [mA]. */
	uint32_t i_run_ma;       /**< Active run current [mA]. */
	uint32_t i_hold_ma;      /**< Active hold current [mA]. */
	uint32_t vbus_mv;        /**< Measured bus voltage [mV]. */
};

/** Slow loop: trajectory, hold current, delivery of the events. @p dev is the parent node. */
int sstep_motor_process(const struct device *dev);

/** Status of the last periods. @p dev is the parent node. */
int sstep_motor_get_status(const struct device *dev, struct sstep_motor_status *status);

/** Change run and hold currents [mA]; limited to the values of the node, hold to run. */
int sstep_motor_set_current(const struct device *dev, uint32_t run_ma, uint32_t hold_ma);

/** Clear the faults; the driver goes to disabled. */
int sstep_motor_clear_faults(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_DRIVERS_SSTEP_MOTOR_H_ */
