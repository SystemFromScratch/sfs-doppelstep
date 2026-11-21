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
 *
 * With an encoder (chosen sfs,encoder, any sensor with SENSOR_CHAN_ROTATION) it reads
 * the angle with every publication of the status and acts on what enc_track.h decides:
 * the commanded position follows the shaft while the outputs are off and is aligned
 * after the enable, lost steps are counted, and the regulator corrects the position at
 * standstill. At the start the motor coordinate becomes the absolute shaft angle.
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
#include "supervisor.h"

#define HAS_ENCODER DT_HAS_CHOSEN(sfs_encoder)
#if HAS_ENCODER
#include <zephyr/drivers/sensor.h>

#include "enc_track.h"
#endif

#define MOTOR_NODE DT_CHOSEN(sfs_motor)
#define SLOW_HZ    DT_PROP(MOTOR_NODE, slow_loop_hz)

BUILD_ASSERT((CONFIG_SYS_CLOCK_TICKS_PER_SEC % SLOW_HZ) == 0,
	     "the system tick rate must be a multiple of the slow loop rate");

/* Slow-loop periods between two publications of the status */
#define STATUS_PERIODS MAX(1, (CONFIG_SFS_STATUS_PERIOD_MS * SLOW_HZ) / 1000)

static const struct device *const motor = DEVICE_DT_GET(MOTOR_NODE);
static const struct device *const motor_drv = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, driver));
static const struct device *const motor_ctrl = DEVICE_DT_GET(DT_CHILD(MOTOR_NODE, ctrl));

#if HAS_ENCODER
#define MS_TO_PUBLICATIONS(ms) MAX(1, (ms) / MAX(1, CONFIG_SFS_STATUS_PERIOD_MS))

static const struct device *const encoder = DEVICE_DT_GET(DT_CHOSEN(sfs_encoder));
static const struct enc_track_cfg enc_cfg = {
	.usteps_per_rev = DT_PROP(MOTOR_NODE, full_steps_per_rev) * DT_PROP(MOTOR_NODE, microsteps),
	.lost_threshold = CONFIG_SFS_ENC_LOST_STEP_USTEPS,
	.settle_periods = MS_TO_PUBLICATIONS(CONFIG_SFS_ENC_SETTLE_MS),
	.correct_periods = MS_TO_PUBLICATIONS(CONFIG_SFS_ENC_CORRECT_MS),
	.correction_max = CONFIG_SFS_ENC_CORRECTION_MAX_USTEPS,
	.fault_reads = 10U,
};
static struct enc_track enc;
static uint32_t enc_gain_milli; /* Position P Gain * 1000 / 1024 */

/* Angle [1/65536 turn] through the sensor API: degrees and micro-degrees */
static int encoder_read(uint16_t *raw)
{
	struct sensor_value v;
	int ret = sensor_sample_fetch_chan(encoder, SENSOR_CHAN_ROTATION);

	if (ret == 0) {
		ret = sensor_channel_get(encoder, SENSOR_CHAN_ROTATION, &v);
	}
	if (ret == 0) {
		const int64_t udeg = (int64_t)v.val1 * 1000000 + v.val2;

		*raw = (uint16_t)((udeg * ENC_COUNTS_PER_REV + 180000000) / 360000000);
	}
	return ret;
}

/* The first good reading: the motor coordinate becomes the absolute shaft angle */
static void encoder_start(void)
{
	uint16_t raw;

	if (enc.valid || enc.fault || !device_is_ready(encoder)) {
		return;
	}
	if (encoder_read(&raw) != 0) {
		enc_track_read_failed(&enc);
		return;
	}
	(void)stepper_ctrl_set_reference_position(motor_ctrl, enc_track_init(&enc, &enc_cfg, raw));
}

/* Read, decide, act; before every periodic publication. true: the motor driver acted. */
static bool encoder_update(const struct sstep_motor_status *st)
{
	uint16_t raw;
	int32_t arg = 0;

	if (!enc.valid) {
		encoder_start();
		return enc.valid;
	}
	if (encoder_read(&raw) != 0) {
		enc_track_read_failed(&enc);
		return false;
	}
	enc_track_sample(&enc, raw);

	switch (enc_track_step(&enc, st->state, st->position, enc_gain_milli, &arg)) {
	case ENC_FOLLOW:
		(void)stepper_ctrl_set_reference_position(motor_ctrl, arg);
		return true;
	case ENC_CORRECT:
		/* with the ramp of the last move: a few microsteps */
		(void)stepper_ctrl_move_to(motor_ctrl, arg);
		return true;
	default:
		return false; /* ENC_LOST: the counter in the status tells the bridge */
	}
}

static void encoder_status(struct app_enc_status *out)
{
	*out = (struct app_enc_status){
		.valid = enc.valid,
		.fault = enc.fault,
		.correcting = enc.correcting,
		.position = enc.position,
		.lost = enc.lost,
	};
}
#endif /* HAS_ENCODER */

static int configure_ramp(uint32_t speed, uint32_t accel)
{
	const struct stepper_ctrl_ramp ramp = {
		.speed_max = speed,
		.acceleration_max = accel,
		.deceleration_max = 0U,
	};

	return stepper_ctrl_configure_ramp(motor_ctrl, &ramp);
}

#if HAS_ENCODER
/* What a command means for the encoder: goal of the regulator, coordinate shift */
static void encoder_command(const struct app_cmd *cmd, int err, int32_t before)
{
	int32_t after;

	if (err != 0) {
		return;
	}
	switch (cmd->op) {
	case APP_CMD_ENABLE:
		/* An XM430 copies Present Position into Goal Position: hold it */
		if (enc.valid) {
			enc_track_set_goal(&enc, enc.position);
		}
		break;
	case APP_CMD_MOVE_TO:
		enc_track_set_goal(&enc, cmd->arg0);
		break;
	case APP_CMD_SET_POSITION:
		if (stepper_ctrl_get_actual_position(motor_ctrl, &after) == 0) {
			enc_track_shift(&enc, after - before);
		}
		break;
	case APP_CMD_SET_GAIN:
		break;
	default: /* run, stop, disable, clear faults */
		enc_track_clear_goal(&enc);
		break;
	}
}
#endif

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
	case APP_CMD_SET_GAIN:
#if HAS_ENCODER
		enc_gain_milli = (cmd->arg1 * 1000U + 512U) / 1024U;
#endif
		return 0;
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

static void publish_status(bool periodic)
{
	struct app_status status = {0};

	if (sstep_motor_get_status(motor, &status.motor) != 0) {
		return;
	}
#if HAS_ENCODER
	/* An action changes the commanded position or the state: publish the new one */
	if (periodic && encoder_update(&status.motor)) {
		(void)sstep_motor_get_status(motor, &status.motor);
	}
	encoder_status(&status.enc);
#else
	ARG_UNUSED(periodic);
#endif
	app_status_publish(&status);
}

static void control_task(void *p1, void *p2, void *p3)
{
	const k_ticks_t period = CONFIG_SYS_CLOCK_TICKS_PER_SEC / SLOW_HZ;
	k_ticks_t next = k_uptime_ticks();
	uint32_t periods = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

#if HAS_ENCODER
	encoder_start();
#endif
	/* The status exists before anyone can read it */
	publish_status(false);

	for (;;) {
		struct app_cmd cmd;

		next += period;
		while (app_cmd_next(&cmd, K_TIMEOUT_ABS_TICKS(next)) == 0) {
#if HAS_ENCODER
			int32_t before = 0;

			(void)stepper_ctrl_get_actual_position(motor_ctrl, &before);
#endif
			const int err = execute(&cmd);

#if HAS_ENCODER
			encoder_command(&cmd, err, before);
#endif
			/* The sender sees a status that reflects its command */
			publish_status(false);
			app_cmd_done(&cmd, err);
		}

		(void)sstep_motor_process(motor);
		supervisor_checkin(SUPERVISOR_CONTROL);

		periods++;
		if (periods >= STATUS_PERIODS) {
			periods = 0U;
			publish_status(true);
		}
	}
}

K_THREAD_DEFINE(control_task_id, CONFIG_SFS_CONTROL_THREAD_STACK_SIZE, control_task, NULL, NULL,
		NULL, CONFIG_SFS_CONTROL_THREAD_PRIORITY,
		COND_CODE_1(CONFIG_FPU_SHARING, (K_FP_REGS), (0)), K_TICKS_FOREVER);
