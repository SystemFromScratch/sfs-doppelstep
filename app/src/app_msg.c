/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See app_msg.h.
 */

#include "app_msg.h"

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(app_msg, CONFIG_APP_MSG_LOG_LEVEL);

K_MSGQ_DEFINE(cmd_q, sizeof(struct app_cmd), 2, 4);
K_MSGQ_DEFINE(rsp_q, sizeof(struct app_rsp), 2, 4);
K_MSGQ_DEFINE(status_q, sizeof(struct sstep_motor_status), 1, 4);

static atomic_t rsp_dropped;

int app_call(enum app_cmd_op op, int32_t arg0, uint32_t arg1, uint32_t arg2)
{
	static uint32_t seq;
	const struct app_cmd cmd = {
		.seq = ++seq,
		.op = op,
		.arg0 = arg0,
		.arg1 = arg1,
		.arg2 = arg2,
	};
	const k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_SFS_CMD_TIMEOUT_MS));

	if (k_msgq_put(&cmd_q, &cmd, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	for (;;) {
		struct app_rsp rsp;

		if (k_msgq_get(&rsp_q, &rsp, sys_timepoint_timeout(end)) != 0) {
			LOG_ERR("command %d: no result", (int)op);
			return -ETIMEDOUT;
		}
		if (rsp.seq == cmd.seq) {
			return rsp.err;
		}
		/* the late answer of a command that timed out */
	}
}

int app_cmd_next(struct app_cmd *cmd, k_timeout_t timeout)
{
	return k_msgq_get(&cmd_q, cmd, timeout);
}

void app_cmd_done(const struct app_cmd *cmd, int err)
{
	const struct app_rsp rsp = {.seq = cmd->seq, .err = err};

	if (k_msgq_put(&rsp_q, &rsp, K_NO_WAIT) != 0) {
		(void)atomic_inc(&rsp_dropped);
	}
}

void app_status_publish(const struct sstep_motor_status *status)
{
	/* Safe between the two calls: the publisher is a cooperative thread that outranks
	 * every reader on a single core; a reader that still sees the queue empty keeps
	 * its previous copy.
	 */
	k_msgq_purge(&status_q);
	(void)k_msgq_put(&status_q, status, K_NO_WAIT);
}

int app_status_get(struct sstep_motor_status *status)
{
	return k_msgq_peek(&status_q, status);
}

uint32_t app_rsp_dropped(void)
{
	return (uint32_t)atomic_get(&rsp_dropped);
}
