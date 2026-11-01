/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stepper-C integration (see motor.h). Execution model of the library
 * (sstep.h): sstep_fast_loop() in the PWM/ADC interrupt at the PWM rate,
 * sstep_slow_loop() and every command from one context at slow_loop_hz. Here
 * that context is the cooperative thread "motor"; it wakes on an absolute
 * tick schedule, so the period does not drift with the work done in it.
 */

#include "motor.h"

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "power_stage.h"
#if defined(CONFIG_SFS_SIM_PLANT)
#include "sim_plant.h"
#endif

LOG_MODULE_REGISTER(motor, CONFIG_SFS_LOG_LEVEL);

#define SLOW_HZ CONFIG_SFS_CTRL_SLOW_LOOP_HZ

BUILD_ASSERT((CONFIG_SYS_CLOCK_TICKS_PER_SEC % SLOW_HZ) == 0,
	     "the system tick rate must be a multiple of the slow loop rate");

enum motor_op {
	OP_ENABLE,
	OP_DISABLE,
	OP_MOVE_TO,
	OP_RUN,
	OP_STOP,
	OP_SET_POSITION,
	OP_SET_CURRENT,
	OP_CLEAR_FAULTS,
};

struct motor_req {
	enum motor_op op;
	int32_t arg0;
	uint32_t arg1;
	uint32_t arg2;
	int result;
	struct k_sem done;
};

K_MSGQ_DEFINE(motor_queue, sizeof(struct motor_req *), 4, sizeof(void *));
K_MUTEX_DEFINE(motor_req_lock); /* one request in flight per caller set */

static struct sstep drv;
static struct k_spinlock snap_lock;
static struct motor_snapshot snap;
static atomic_t fault_flags; /* set by on_fault (possibly from the ISR) */

static K_THREAD_STACK_DEFINE(motor_stack, CONFIG_SFS_MOTOR_THREAD_STACK_SIZE);
static struct k_thread motor_thread;

/* ------------------------------------------------------------------------ */
/* Stepper-C callbacks                                                      */

static void on_fault(void *context, uint16_t faults)
{
	ARG_UNUSED(context);
	/* May run in the zero-latency interrupt: no kernel calls here. */
	(void)atomic_or(&fault_flags, (atomic_val_t)faults);
}

static void on_event(void *context, enum sstep_event event)
{
	ARG_UNUSED(context);
	if (event == SSTEP_EVENT_MOVE_DONE) {
		LOG_DBG("move done");
	}
}

static void fast_loop(const struct sstep_sample *sample, bool w_trip, void *ctx)
{
	struct sstep *const d = ctx;

	if (w_trip) {
		(void)sstep_report_fault(d, SSTEP_FAULT_EXTERNAL);
	}
	(void)sstep_fast_loop(d, sample);
}

/* ------------------------------------------------------------------------ */

static int build_config(struct sstep_config *cfg)
{
	const struct power_stage_info *ps = power_stage_get_info();

	if ((ps->pwm_freq_hz % SLOW_HZ) != 0U) {
		LOG_ERR("PWM %u Hz is not a multiple of the slow loop %u Hz", ps->pwm_freq_hz,
			SLOW_HZ);
		return -EINVAL;
	}

	*cfg = (struct sstep_config){
		.motor = {
			.r_mohm = CONFIG_SFS_MOTOR_R_MOHM,
			.l_uh = CONFIG_SFS_MOTOR_L_UH,
			.i_rated_ma = CONFIG_SFS_MOTOR_I_RATED_MA,
			.ke_uv_s_per_rad = CONFIG_SFS_MOTOR_KE_UV_S_PER_RAD,
			.full_steps_per_rev = CONFIG_SFS_MOTOR_FULL_STEPS_PER_REV,
		},
		.stage = {
			.topology = SSTEP_TOPOLOGY_SHARED_LEG,
			.vbus_nominal_mv = CONFIG_SFS_STAGE_VBUS_NOMINAL_MV,
			.vbus_min_mv = CONFIG_SFS_STAGE_VBUS_MIN_MV,
			.vbus_max_mv = CONFIG_SFS_STAGE_VBUS_MAX_MV,
			.i_full_scale_ma = ps->i_full_scale_ma,
			.i_trip_ma = CONFIG_SFS_STAGE_I_TRIP_MA,
			.pwm_period_ticks = ps->pwm_period_ticks,
			.pwm_freq_hz = ps->pwm_freq_hz,
			.dead_time_ns = ps->dead_time_ns,
			.adc_i_zero = ps->adc_i_zero,
			.adc_i_span = ps->adc_i_span,
			.adc_vbus_mv_full = ps->adc_vbus_mv_full,
			.adc_full = ps->adc_full,
		},
		.ctrl = {
			.drive_mode = IS_ENABLED(CONFIG_SFS_CTRL_VOLTAGE_DRIVE) ? SSTEP_DRIVE_VOLTAGE
										: SSTEP_DRIVE_CURRENT,
			/* one injected sequence per PWM period */
			.fast_loop_hz = ps->pwm_freq_hz,
			.slow_loop_hz = SLOW_HZ,
			.microsteps = CONFIG_SFS_CTRL_MICROSTEPS,
			.current_bw_hz = CONFIG_SFS_CTRL_CURRENT_BW_HZ,
			.i_run_ma = CONFIG_SFS_CTRL_I_RUN_MA,
			.i_hold_ma = CONFIG_SFS_CTRL_I_HOLD_MA,
			.hold_delay_ms = CONFIG_SFS_CTRL_HOLD_DELAY_MS,
			.v_max = CONFIG_SFS_CTRL_V_MAX,
			.accel = CONFIG_SFS_CTRL_ACCEL_MAX,
			.decel = 0U,
		},
		.hal = {
			.context = NULL,
			.pwm_write = power_stage_pwm_write,
			.output_enable = power_stage_output_enable,
			.on_fault = on_fault,
			.on_event = on_event,
		},
	};
	return 0;
}

static int sstep_to_errno(int rc)
{
	switch (rc) {
	case SSTEP_OK:
		return 0;
	case SSTEP_ERR_STATE:
		return -EBUSY;
	case SSTEP_ERR_UNSUPPORTED:
		return -ENOTSUP;
	default:
		return -EINVAL;
	}
}

/* Runs in the slow-loop thread. */
static int execute(struct motor_req *req)
{
	int rc;

	switch (req->op) {
	case OP_ENABLE:
		rc = sstep_enable(&drv);
		break;
	case OP_DISABLE:
		rc = sstep_disable(&drv);
		break;
	case OP_MOVE_TO:
		rc = sstep_set_motion_limits(&drv, req->arg1, req->arg2, 0U);
		if (rc == SSTEP_OK) {
			rc = sstep_move_to(&drv, req->arg0);
		}
		break;
	case OP_RUN:
		rc = sstep_set_motion_limits(&drv, req->arg1, req->arg2, 0U);
		if (rc == SSTEP_OK) {
			rc = sstep_run(&drv, req->arg0);
		}
		break;
	case OP_STOP:
		rc = sstep_stop(&drv);
		break;
	case OP_SET_POSITION: {
		struct sstep_status before;

		(void)sstep_get_status(&drv, &before);
		rc = sstep_set_position(&drv, req->arg0);
#if defined(CONFIG_SFS_SIM_PLANT)
		if (rc == SSTEP_OK) {
			/* the rotor stays where it is, only its coordinate changes */
			sim_plant_shift_position(req->arg0 - before.position);
		}
#endif
		break;
	}
	case OP_SET_CURRENT:
		rc = sstep_set_current(&drv, req->arg1, req->arg2);
		if (rc == SSTEP_OK) {
			k_spinlock_key_t key = k_spin_lock(&snap_lock);

			snap.i_run_ma = req->arg1;
			snap.i_hold_ma = req->arg2;
			k_spin_unlock(&snap_lock, key);
		}
		break;
	case OP_CLEAR_FAULTS:
		rc = sstep_clear_faults(&drv);
		(void)atomic_clear(&fault_flags);
		break;
	default:
		rc = SSTEP_ERR_PARAM;
		break;
	}
	return sstep_to_errno(rc);
}

static void publish(void)
{
	struct sstep_status status;
	k_spinlock_key_t key;

	if (sstep_get_status(&drv, &status) != SSTEP_OK) {
		return;
	}

	const float ia = (float)status.i_a_ma;
	const float ib = (float)status.i_b_ma;
	const uint32_t amplitude = (uint32_t)sqrtf((ia * ia) + (ib * ib));

	key = k_spin_lock(&snap_lock);
	snap.status = status;
	snap.i_amplitude_ma = amplitude;
	snap.sequence++;
	k_spin_unlock(&snap_lock, key);
}

static void motor_thread_fn(void *p1, void *p2, void *p3)
{
	const k_ticks_t period = CONFIG_SYS_CLOCK_TICKS_PER_SEC / SLOW_HZ;
	k_ticks_t next = k_uptime_ticks();
	uint16_t reported = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct motor_req *req;

		next += period;
		(void)k_sleep(K_TIMEOUT_ABS_TICKS(next));

		(void)sstep_slow_loop(&drv);

		while (k_msgq_get(&motor_queue, &req, K_NO_WAIT) == 0) {
			req->result = execute(req);
			k_sem_give(&req->done);
		}

		publish();

		const uint16_t faults = (uint16_t)atomic_get(&fault_flags);

		if (faults != reported) {
			if (faults != 0U) {
				LOG_WRN("faults 0x%04x", faults);
			}
			reported = faults;
		}
	}
}

/* ------------------------------------------------------------------------ */

static int request(enum motor_op op, int32_t arg0, uint32_t arg1, uint32_t arg2)
{
	struct motor_req req = {.op = op, .arg0 = arg0, .arg1 = arg1, .arg2 = arg2};
	struct motor_req *ptr = &req;
	int ret;

	if (k_current_get() == &motor_thread) {
		return execute(&req);
	}

	k_sem_init(&req.done, 0, 1);
	(void)k_mutex_lock(&motor_req_lock, K_FOREVER);
	ret = k_msgq_put(&motor_queue, &ptr, K_MSEC(10));
	if (ret == 0) {
		/* the slow loop executes it within one period */
		ret = k_sem_take(&req.done, K_MSEC(50));
		if (ret == 0) {
			ret = req.result;
		} else {
			LOG_ERR("command %d not executed", op);
			/* req lives on this stack: wait until the thread is done with it */
			(void)k_sem_take(&req.done, K_FOREVER);
			ret = -ETIMEDOUT;
		}
	}
	k_mutex_unlock(&motor_req_lock);
	return ret;
}

int motor_init(void)
{
	struct sstep_config cfg;
	int ret;

	ret = build_config(&cfg);
	if (ret != 0) {
		return ret;
	}

	ret = sstep_init(&drv, &cfg);
	if (ret != SSTEP_OK) {
		LOG_ERR("sstep_init: %d", ret);
		return sstep_to_errno(ret);
	}

	snap.i_run_ma = cfg.ctrl.i_run_ma;
	snap.i_hold_ma = cfg.ctrl.i_hold_ma;
	publish();

	power_stage_set_w_trip(IS_ENABLED(CONFIG_SFS_STAGE_PHASE_W_TRIP) ? cfg.stage.i_trip_ma
									  : 0U);
	power_stage_set_fast_fn(fast_loop, &drv);

	(void)k_thread_create(&motor_thread, motor_stack, K_THREAD_STACK_SIZEOF(motor_stack),
			      motor_thread_fn, NULL, NULL, NULL, CONFIG_SFS_MOTOR_THREAD_PRIORITY,
			      K_FP_REGS, K_NO_WAIT);
	(void)k_thread_name_set(&motor_thread, "motor");

	LOG_INF("Stepper-C %s: %u microsteps, run %u mA, hold %u mA, %s drive", SSTEP_VERSION,
		cfg.ctrl.microsteps, cfg.ctrl.i_run_ma, cfg.ctrl.i_hold_ma,
		(cfg.ctrl.drive_mode == SSTEP_DRIVE_CURRENT) ? "current" : "voltage");
	return 0;
}

void motor_get_snapshot(struct motor_snapshot *snapshot)
{
	k_spinlock_key_t key = k_spin_lock(&snap_lock);

	*snapshot = snap;
	k_spin_unlock(&snap_lock, key);
}

bool motor_outputs_allowed(void)
{
	/* the simulated motor never enables the gate drivers */
	return IS_ENABLED(CONFIG_SFS_SIM_PLANT) || (power_stage_get_info()->dead_time_ns != 0U);
}

int motor_enable(bool enable)
{
	if (enable && !motor_outputs_allowed()) {
		LOG_ERR("enable refused: dead-time-ns of the power stage is not set");
		return -EPERM;
	}
	return request(enable ? OP_ENABLE : OP_DISABLE, 0, 0U, 0U);
}

int motor_move_to(int32_t target, uint32_t v_max, uint32_t accel)
{
	return request(OP_MOVE_TO, target, v_max, accel);
}

int motor_run(int32_t velocity, uint32_t v_max, uint32_t accel)
{
	return request(OP_RUN, velocity, v_max, accel);
}

int motor_stop(void)
{
	return request(OP_STOP, 0, 0U, 0U);
}

int motor_set_position(int32_t position)
{
	return request(OP_SET_POSITION, position, 0U, 0U);
}

int motor_set_current(uint32_t run_ma, uint32_t hold_ma)
{
	run_ma = MIN(run_ma, (uint32_t)CONFIG_SFS_CTRL_I_RUN_MA);
	hold_ma = MIN(MIN(hold_ma, (uint32_t)CONFIG_SFS_CTRL_I_HOLD_MA), run_ma);
	return request(OP_SET_CURRENT, 0, run_ma, hold_ma);
}

int motor_clear_faults(void)
{
	return request(OP_CLEAR_FAULTS, 0, 0U, 0U);
}
