/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Control task: the slow loop of the motor driver at the rate of its node, the
 * commands of the communication task and the publication of the status. It is the
 * only thread that calls the motor driver. Cooperative, so another thread never
 * preempts the slow loop.
 *
 * It waits for a command until the next period starts, on an absolute tick schedule,
 * so a command executes on receipt and the period does not drift with the work done.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/kernel.h>

#include <doppelstep/drivers/sstep_motor.h>

#include "app_msg.h"
#include "app_tasks.h"

#define MOTOR_NODE DT_CHOSEN(sfs_motor)
#define SLOW_HZ    DT_PROP(MOTOR_NODE, slow_loop_hz)

BUILD_ASSERT((CONFIG_SYS_CLOCK_TICKS_PER_SEC % SLOW_HZ) == 0,
	     "the system tick rate must be a multiple of the slow loop rate");

/* Slow-loop periods between two publications of the status */
#define STATUS_PERIODS MAX(1, (CONFIG_SFS_STATUS_PERIOD_MS * SLOW_HZ) / 1000)

static const struct device *const motor = DEVICE_DT_GET(MOTOR_NODE);
static const struct device *const motor_drv = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, driver));
static const struct device *const motor_ctrl = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, ctrl));

static int configure_ramp(uint32_t speed, uint32_t accel)
{
	const struct stepper_ctrl_ramp ramp = {
		.speed_max = speed,
		.acceleration_max = accel,
		.deceleration_max = 0U,
	};

	return stepper_ctrl_configure_ramp(motor_ctrl, &ramp);
}

static int execute(const struct app_cmd *cmd)
{
	int ret;

	switch (cmd->op) {
	case APP_CMD_ENABLE:
		return stepper_enable(motor_drv);
	case APP_CMD_DISABLE:
		return stepper_disable(motor_drv);
	case APP_CMD_MOVE_TO:
		ret = configure_ramp(cmd->arg1, cmd->arg2);
		return (ret == 0) ? stepper_ctrl_move_to(motor_ctrl, cmd->arg0) : ret;
	case APP_CMD_RUN:
		ret = configure_ramp(cmd->arg1, cmd->arg2);
		if (ret != 0) {
			return ret;
		}
		if (cmd->arg0 == 0) {
			return stepper_ctrl_stop(motor_ctrl);
		}
		return stepper_ctrl_run(motor_ctrl, (cmd->arg0 < 0)
							    ? STEPPER_CTRL_DIRECTION_NEGATIVE
							    : STEPPER_CTRL_DIRECTION_POSITIVE);
	case APP_CMD_STOP:
		return stepper_ctrl_stop(motor_ctrl);
	case APP_CMD_SET_POSITION:
		return stepper_ctrl_set_reference_position(motor_ctrl, cmd->arg0);
	case APP_CMD_SET_CURRENT:
		return sstep_motor_set_current(motor, cmd->arg1, cmd->arg2);
	case APP_CMD_CLEAR_FAULTS:
		return sstep_motor_clear_faults(motor);
	default:
		return -EINVAL;
	}
}

static void publish_status(void)
{
	struct sstep_motor_status status;

	if (sstep_motor_get_status(motor, &status) == 0) {
		app_status_publish(&status);
	}
}

static void control_task(void *p1, void *p2, void *p3)
{
	const k_ticks_t period = CONFIG_SYS_CLOCK_TICKS_PER_SEC / SLOW_HZ;
	k_ticks_t next = k_uptime_ticks();
	uint32_t periods = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* The status exists before anyone can read it */
	publish_status();

	for (;;) {
		struct app_cmd cmd;

		next += period;
		while (app_cmd_next(&cmd, K_TIMEOUT_ABS_TICKS(next)) == 0) {
			const int err = execute(&cmd);

			/* The sender sees a status that reflects its command */
			publish_status();
			app_cmd_done(&cmd, err);
		}

		(void)sstep_motor_process(motor);

		periods++;
		if (periods >= STATUS_PERIODS) {
			periods = 0U;
			publish_status();
		}
	}
}

K_THREAD_DEFINE(control_task_id, CONFIG_SFS_CONTROL_THREAD_STACK_SIZE, control_task, NULL, NULL,
		NULL, CONFIG_SFS_CONTROL_THREAD_PRIORITY,
		COND_CODE_1(CONFIG_FPU_SHARING, (K_FP_REGS), (0)), K_TICKS_FOREVER);
