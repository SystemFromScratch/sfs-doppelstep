/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The queues between the communication and the control task. Messages travel by
 * value. The control task never waits on an outgoing queue, so the communication
 * side cannot stall the control loop.
 *
 *   comm task     app_call() ---- cmd_q ---->  app_cmd_next()   control task
 *                            <--- rsp_q -----  app_cmd_done()
 *   any reader    app_status_get() <- status_q - app_status_publish()
 */

#ifndef DOPPELSTEP_APP_MSG_H_
#define DOPPELSTEP_APP_MSG_H_

#include <stdint.h>

#include <zephyr/kernel.h>

#include <doppelstep/drivers/sstep_motor.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_cmd_op {
	APP_CMD_ENABLE,       /**< Outputs on, hold the position. */
	APP_CMD_DISABLE,      /**< Outputs off. */
	APP_CMD_MOVE_TO,      /**< arg0 target [microsteps], arg1 speed, arg2 acceleration. */
	APP_CMD_RUN,          /**< arg0 velocity [microsteps/s], arg1 speed, arg2 acceleration. */
	APP_CMD_STOP,         /**< Decelerate to standstill. */
	APP_CMD_SET_POSITION, /**< arg0 position [microsteps]; the rotor does not move. */
	APP_CMD_SET_CURRENT,  /**< arg1 run, arg2 hold [mA]. */
	APP_CMD_CLEAR_FAULTS, /**< Clear the faults; outputs stay off. */
};

struct app_cmd {
	uint32_t seq; /**< Set by app_call(). */
	enum app_cmd_op op;
	int32_t arg0;
	uint32_t arg1;
	uint32_t arg2;
};

struct app_rsp {
	uint32_t seq; /**< Of the command. */
	int err;      /**< 0 or -errno. */
};

extern struct k_msgq cmd_q;
extern struct k_msgq rsp_q;
extern struct k_msgq status_q;

/**
 * @brief Send a command and wait for its result, at most CONFIG_SFS_CMD_TIMEOUT_MS.
 *        Communication task only: one command is in flight at a time.
 * @return The result of the control side, -EBUSY when the queue is full, -ETIMEDOUT
 *         when no answer came; the late answer is discarded by a later call.
 */
int app_call(enum app_cmd_op op, int32_t arg0, uint32_t arg1, uint32_t arg2);

/** Control task: next command; returns as k_msgq_get(). */
int app_cmd_next(struct app_cmd *cmd, k_timeout_t timeout);

/** Control task: result of a command. Never blocks; a full queue counts a drop. */
void app_cmd_done(const struct app_cmd *cmd, int err);

/** Control task: publish the status; it replaces the previous one. */
void app_status_publish(const struct sstep_motor_status *status);

/** Latest status, any thread. @return 0, or -ENOMSG before the first publication. */
int app_status_get(struct sstep_motor_status *status);

/** Results dropped by app_cmd_done() since start. */
uint32_t app_rsp_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_APP_MSG_H_ */
