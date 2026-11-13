/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stepper-C behind Zephyr's stepper classes, see sstep_motor.h. Execution model of
 * the library (sstep.h): sstep_fast_loop() in the PWM/ADC interrupt at the PWM rate,
 * sstep_slow_loop() and every command from one context at slow_loop_hz.
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <sstep.h>

#include <doppelstep/drivers/power_stage.h>
#include <doppelstep/drivers/sstep_motor.h>

#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
/* The test takes about 40 to 55 ms, at most about 440 ms while a rotor keeps moving
 * (Stepper-C 1.4.0). stepper_enable() does not wait for it: the slow loop
 * ends it, so a DYNAMIXEL host gets the answer to Torque Enable at once, as from an
 * XM430.
 */
#define SELFTEST_TIMEOUT_MS 500U

/* A motion command received while the self-test runs, executed when it passes */
enum queued_op {
	QUEUED_NONE,
	QUEUED_MOVE_TO,
	QUEUED_MOVE_BY,
	QUEUED_RUN,
	QUEUED_STOP,
};
#endif

LOG_MODULE_REGISTER(sstep_motor, CONFIG_SSTEP_MOTOR_LOG_LEVEL);

BUILD_ASSERT((int)SSTEP_MOTOR_DISABLED == (int)SSTEP_STATE_DISABLED);
BUILD_ASSERT((int)SSTEP_MOTOR_HOLDING == (int)SSTEP_STATE_HOLDING);
BUILD_ASSERT((int)SSTEP_MOTOR_MOVING == (int)SSTEP_STATE_MOVING);
BUILD_ASSERT((int)SSTEP_MOTOR_FAULT == (int)SSTEP_STATE_FAULT);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_OVERCURRENT == SSTEP_FAULT_OVERCURRENT);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_OVERVOLTAGE == SSTEP_FAULT_OVERVOLTAGE);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_UNDERVOLTAGE == SSTEP_FAULT_UNDERVOLTAGE);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_HAL == SSTEP_FAULT_HAL);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_EXTERNAL == SSTEP_FAULT_EXTERNAL);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_STALL == SSTEP_FAULT_STALL);
BUILD_ASSERT(SSTEP_MOTOR_FAULT_SELFTEST == SSTEP_FAULT_SELFTEST);

struct sstep_motor_config {
	const struct device *stage;
	struct sstep_motor_cfg motor;
	uint32_t vbus_nominal_mv;
	uint32_t vbus_min_mv;
	uint32_t vbus_max_mv;
	uint32_t i_trip_ma;
	bool aux_trip;
	bool voltage_drive;
	uint32_t slow_loop_hz;
	uint32_t microsteps;
	uint32_t current_bw_hz;
	uint32_t i_run_ma;
	uint32_t i_hold_ma;
	uint32_t hold_delay_ms;
	uint32_t v_max;
	uint32_t accel_max;
	uint32_t i_selftest_ma;
	uint32_t i_slew_ma_per_ms;
};

struct sstep_motor_data {
	struct sstep drv;
	struct power_stage_info stage_info;

	/* Set by on_fault, possibly from an interrupt above the kernel's lock */
	atomic_t fault_flags;
	uint16_t faults_reported;
	/* Outputs switched on by Stepper-C (hal_output_enable), read by the fast loop */
	atomic_t outputs_on;
	const struct device *stage;

#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	/* sstep_selftest() of Stepper-C runs; the fast loop checks the shunt of the
	 * shared leg meanwhile (Kirchhoff: iU + iV + iW = 0), which Stepper-C does not see.
	 */
	atomic_t selftest_active;
	/* |iU + iV + iW| above kirchhoff_limit for kirchhoff_fail periods in a row is a
	 * broken sensor: a broken sensor is wrong as long as current flows, a disturbed
	 * sample (a switching edge) only once.
	 */
	int32_t kirchhoff_limit;    /* a quarter of the test current [counts] */
	uint32_t kirchhoff_fail;    /* 2 ms in fast-loop periods */
	uint32_t kirchhoff_run;     /* periods in a row above the limit */
	uint32_t kirchhoff_run_max; /* longest run of the test */
	int32_t kirchhoff_peak;     /* largest |iU + iV + iW| of the test [counts] */
	bool selftest_passed;
	/* stepper_enable() accepted, the self-test runs, Stepper-C enables after it */
	bool enable_pending;
	uint32_t pending_calls; /* slow-loop periods since the test started */
	enum queued_op queued_op;
	int32_t queued_arg;
#endif

	/* Filtered current amplitude [mA * 16], see sstep_motor_get_status() */
	uint32_t i_amp_q4;

	uint32_t speed;
	uint32_t accel;
	uint32_t decel;
	uint32_t i_run_ma;
	uint32_t i_hold_ma;

	stepper_event_cb_t drv_cb;
	void *drv_cb_data;
	const struct device *drv_dev;
	stepper_ctrl_event_callback_t ctrl_cb;
	void *ctrl_cb_data;
	const struct device *ctrl_dev;

#if defined(CONFIG_SSTEP_MOTOR_THREAD_CHECK)
	k_tid_t owner;
#endif
};

/* Child nodes */
struct sstep_motor_child_config {
	const struct device *parent;
};

#if defined(CONFIG_SSTEP_MOTOR_THREAD_CHECK)
#define CHECK_CALLER(data)                                                                         \
	__ASSERT(((data)->owner == NULL) || ((data)->owner == k_current_get()),                    \
		 "sstep_motor: every call must come from one thread")
#else
#define CHECK_CALLER(data)
#endif

static int to_errno(int rc)
{
	switch (rc) {
	case SSTEP_OK:
		return 0;
	case SSTEP_ERR_STATE:
		return -EBUSY;
	case SSTEP_ERR_UNSUPPORTED:
		return -ENOTSUP;
	case SSTEP_ERR_VBUS:
		return -ERANGE;
	default:
		return -EINVAL;
	}
}

static uint32_t isqrt_u32(uint32_t v)
{
	uint32_t root = 0U;
	uint32_t bit = 1UL << 30;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (v >= (root + bit)) {
			v -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return root;
}

/* ------------------------------------------------------------------------ */
/* Stepper-C callbacks                                                      */

static int hal_pwm_write(void *context, const uint16_t compare[SSTEP_LEGS_MAX])
{
	const struct device *dev = context;
	const struct sstep_motor_config *cfg = dev->config;
	const struct sstep_motor_data *data = dev->data;

	return power_stage_write(cfg->stage, compare, data->stage_info.legs);
}

static int hal_output_enable(void *context, bool enable)
{
	const struct device *dev = context;
	const struct sstep_motor_config *cfg = dev->config;
	struct sstep_motor_data *data = dev->data;
	const int ret = power_stage_output_enable(cfg->stage, enable);

	/* May run in the fast-loop interrupt: atomics only. */
	(void)atomic_set(&data->outputs_on, ((ret == 0) && enable) ? 1 : 0);
	return ret;
}

static void on_fault(void *context, uint16_t faults)
{
	const struct device *dev = context;
	struct sstep_motor_data *data = dev->data;

	/* May run in the fast-loop interrupt: no kernel calls here. */
	(void)atomic_or(&data->fault_flags, (atomic_val_t)faults);
}

/* From sstep_slow_loop(), so in the thread of sstep_motor_process() */
static void on_event(void *context, enum sstep_event event)
{
	const struct device *dev = context;
	struct sstep_motor_data *data = dev->data;

	if ((event == SSTEP_EVENT_MOVE_DONE) && (data->ctrl_cb != NULL)) {
		data->ctrl_cb(data->ctrl_dev, STEPPER_CTRL_EVENT_STEPS_COMPLETED,
			      data->ctrl_cb_data);
	}
}

static void fast_loop(const struct power_stage_sample *s, uint32_t trips, void *ctx)
{
	struct sstep *const drv = ctx;
	const struct sstep_sample sample = {.i_a = s->i_a, .i_b = s->i_b, .vbus = s->vbus};

	if ((trips & POWER_STAGE_TRIP_AUX) != 0U) {
		(void)sstep_report_fault(drv, SSTEP_FAULT_EXTERNAL);
	}
	/* Hardware comparators: the break input has switched the outputs off already */
	if ((trips & POWER_STAGE_TRIP_HW_OC) != 0U) {
		(void)sstep_report_fault(drv, SSTEP_FAULT_OVERCURRENT);
	}
	if ((trips & POWER_STAGE_TRIP_HW_OV) != 0U) {
		(void)sstep_report_fault(drv, SSTEP_FAULT_OVERVOLTAGE);
	}
	/* The stage has switched the outputs off already; the fault keeps them off. With
	 * the outputs off a late period is harmless, for example while a flash write of
	 * the EEPROM area (allowed only with the torque off) stalls the CPU: no fault.
	 */
	if (((trips & POWER_STAGE_TRIP_OVERRUN) != 0U) &&
	    (atomic_get(&CONTAINER_OF(drv, struct sstep_motor_data, drv)->outputs_on) != 0)) {
		(void)sstep_report_fault(drv, SSTEP_FAULT_HAL);
	}
#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	struct sstep_motor_data *const data = CONTAINER_OF(drv, struct sstep_motor_data, drv);

	if (atomic_get(&data->selftest_active) != 0) {
		const int32_t sum = abs((int32_t)s->i_a + (int32_t)s->i_b + (int32_t)s->i_aux -
					(3 * (int32_t)data->stage_info.adc_i_zero));

		data->kirchhoff_peak = MAX(data->kirchhoff_peak, sum);
		data->kirchhoff_run = (sum > data->kirchhoff_limit) ? (data->kirchhoff_run + 1U) : 0U;
		data->kirchhoff_run_max = MAX(data->kirchhoff_run_max, data->kirchhoff_run);
	}
#endif
	(void)sstep_fast_loop(drv, &sample);
}

#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
static void finish_enable(const struct sstep_motor_config *cfg, struct sstep_motor_data *data);
#endif

/* ------------------------------------------------------------------------ */
/* Extension on the parent node                                             */

int sstep_motor_process(const struct device *dev)
{
	struct sstep_motor_data *data = dev->data;
	uint16_t faults;
	uint16_t fresh;
	int rc;

#if defined(CONFIG_SSTEP_MOTOR_THREAD_CHECK)
	if (data->owner == NULL) {
		data->owner = k_current_get();
	}
#endif
	CHECK_CALLER(data);

	rc = sstep_slow_loop(&data->drv);
#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	if (data->enable_pending) {
		finish_enable(dev->config, data);
	}
#endif

	faults = (uint16_t)atomic_get(&data->fault_flags);
	fresh = faults & (uint16_t)~data->faults_reported;
	data->faults_reported = faults;
	if (fresh != 0U) {
		LOG_WRN("faults 0x%04x", faults);
		if (data->drv_cb != NULL) {
			data->drv_cb(data->drv_dev,
				     ((fresh & SSTEP_FAULT_STALL) != 0U)
					     ? STEPPER_EVENT_STALL_DETECTED
					     : STEPPER_EVENT_FAULT_DETECTED,
				     data->drv_cb_data);
		}
	}
	return to_errno(rc);
}

int sstep_motor_get_status(const struct device *dev, struct sstep_motor_status *status)
{
	struct sstep_motor_data *data = dev->data;
	struct sstep_status st;
	int rc;

	CHECK_CALLER(data);

	rc = sstep_get_status(&data->drv, &st);
	if (rc != SSTEP_OK) {
		return to_errno(rc);
	}

#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	/* The outputs are on and the rotor is held while the self-test runs: torque on */
	if (data->enable_pending && (st.state == SSTEP_STATE_DISABLED)) {
		st.state = SSTEP_STATE_HOLDING;
	}
#endif

	const uint32_t ia = (uint32_t)MIN(abs(st.i_a_ma), 32767);
	const uint32_t ib = (uint32_t)MIN(abs(st.i_b_ma), 32767);
	const uint32_t amp = isqrt_u32((ia * ia) + (ib * ib));

	/* One sample of the fast loop moves by an ADC step (29 mA on the ESC1): the
	 * amplitude is averaged over 8 calls (8 ms at the 1 ms status period). With the
	 * outputs off no current flows, whatever the offset drift of the sensors says.
	 */
	if ((st.state == SSTEP_STATE_DISABLED) || (st.state == SSTEP_STATE_FAULT)) {
		data->i_amp_q4 = 0U;
	} else {
		data->i_amp_q4 = data->i_amp_q4 - (data->i_amp_q4 / 8U) + ((amp * 16U) / 8U);
	}

	*status = (struct sstep_motor_status){
		.state = (enum sstep_motor_state)st.state,
		.faults = st.faults,
		.position = st.position,
		.target = st.target,
		.velocity = st.velocity,
		.i_amplitude_ma = (data->i_amp_q4 + 8U) / 16U,
		.i_run_ma = data->i_run_ma,
		.i_hold_ma = data->i_hold_ma,
		.vbus_mv = st.vbus_mv,
	};
	return 0;
}

int sstep_motor_set_current(const struct device *dev, uint32_t run_ma, uint32_t hold_ma)
{
	const struct sstep_motor_config *cfg = dev->config;
	struct sstep_motor_data *data = dev->data;
	const uint32_t run = MIN(run_ma, cfg->i_run_ma);
	const uint32_t hold = MIN(MIN(hold_ma, cfg->i_hold_ma), run);
	int rc;

	CHECK_CALLER(data);

	rc = sstep_set_current(&data->drv, run, hold);
	if (rc == SSTEP_OK) {
		data->i_run_ma = run;
		data->i_hold_ma = hold;
	}
	return to_errno(rc);
}

int sstep_motor_clear_faults(const struct device *dev)
{
	struct sstep_motor_data *data = dev->data;
	const int rc = sstep_clear_faults(&data->drv);

	CHECK_CALLER(data);

	(void)atomic_clear(&data->fault_flags);
	data->faults_reported = 0U;
#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	data->selftest_passed = false; /* test again before the next enable */
	data->enable_pending = false;
	data->queued_op = QUEUED_NONE;
#endif
	return to_errno(rc);
}

/* ------------------------------------------------------------------------ */
/* Class "stepper" on the child sfs,sstep-stepper-driver                    */

#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
static const char *selftest_str(enum sstep_selftest_result r)
{
	switch (r) {
	case SSTEP_SELFTEST_RUNNING:
		return "timeout";
	case SSTEP_SELFTEST_PASS:
		return "iU + iV + iW is not 0 (a current sensor is broken)";
	case SSTEP_SELFTEST_LEAK:
		return "current with all legs low (a switch conducts)";
	case SSTEP_SELFTEST_OPEN:
		return "no current (phase open, motor not connected or leg dead)";
	case SSTEP_SELFTEST_SHORT:
		return "current too high (short circuit)";
	case SSTEP_SELFTEST_POLARITY:
		return "current of the wrong sign (current-sense-invert)";
	case SSTEP_SELFTEST_WRONG_PHASE:
		return "current in the other phase (phase on the wrong legs)";
	case SSTEP_SELFTEST_ABORTED:
		return "aborted by a bus voltage trip or a power stage error";
	default:
		return "not run";
	}
}

static int32_t selftest_counts(const struct sstep_motor_config *cfg,
			       const struct power_stage_info *info)
{
	return (int32_t)MAX((uint64_t)cfg->i_selftest_ma * (uint64_t)abs(info->adc_i_span) /
				    MAX(info->i_full_scale_ma, 1U),
			    4U);
}

/* Starts sstep_selftest() of Stepper-C; -ERANGE for the bus voltage (nothing on) */
static int start_selftest(const struct sstep_motor_config *cfg, struct sstep_motor_data *data)
{
	/* The shared-leg stage measures the third leg: the leg currents must add up to 0 */
	const bool kirchhoff = (data->stage_info.legs == 3U);
	int ret;

	data->kirchhoff_limit = selftest_counts(cfg, &data->stage_info) / 4;
	data->kirchhoff_fail = MAX((2U * data->stage_info.pwm_freq_hz) / 1000U, 1U);
	data->kirchhoff_run = 0U;
	data->kirchhoff_run_max = 0U;
	data->kirchhoff_peak = 0;
	(void)atomic_set(&data->selftest_active, kirchhoff ? 1 : 0);
	ret = to_errno(sstep_selftest(&data->drv, cfg->i_selftest_ma));
	if (ret != 0) {
		(void)atomic_set(&data->selftest_active, 0);
		return ret;
	}
	data->enable_pending = true;
	data->pending_calls = 0U;
	data->queued_op = QUEUED_NONE;
	return 0;
}

static int run_queued(struct sstep_motor_data *data)
{
	switch (data->queued_op) {
	case QUEUED_MOVE_TO:
		return sstep_move_to(&data->drv, data->queued_arg);
	case QUEUED_MOVE_BY:
		return sstep_move_by(&data->drv, data->queued_arg);
	case QUEUED_RUN:
		return sstep_run(&data->drv, data->queued_arg);
	case QUEUED_STOP:
		return sstep_stop(&data->drv);
	default:
		return SSTEP_OK;
	}
}

/* Slow loop: the end of the self-test enables Stepper-C or latches the fault */
static void finish_enable(const struct sstep_motor_config *cfg, struct sstep_motor_data *data)
{
	const struct power_stage_info *info = &data->stage_info;
	const bool kirchhoff = (info->legs == 3U);
	struct sstep_status st;

	(void)sstep_get_status(&data->drv, &st);
	data->pending_calls++;
	/* Counted in slow-loop periods: the test runs in fast-loop periods, not in time */
	if ((st.selftest == SSTEP_SELFTEST_RUNNING) &&
	    (data->pending_calls < ((SELFTEST_TIMEOUT_MS * cfg->slow_loop_hz) / 1000U))) {
		return;
	}
	(void)atomic_set(&data->selftest_active, 0);
	data->enable_pending = false;
	if (st.selftest == SSTEP_SELFTEST_RUNNING) {
		(void)sstep_disable(&data->drv); /* stops the test, outputs off */
	}
	if ((st.selftest == SSTEP_SELFTEST_PASS) &&
	    (!kirchhoff || (data->kirchhoff_run_max < data->kirchhoff_fail))) {
		int rc;

		LOG_INF("selftest passed: %d mA at duty %u/%u, legs sum up to %d counts (limit %d), "
			"%u periods in a row above",
			st.selftest_i_ma, st.selftest_duty, info->pwm_period_ticks,
			data->kirchhoff_peak, data->kirchhoff_limit, data->kirchhoff_run_max);
		data->selftest_passed = true;
		rc = sstep_enable(&data->drv);
		if (rc == SSTEP_OK) {
			rc = run_queued(data);
		}
		data->queued_op = QUEUED_NONE;
		if (rc != SSTEP_OK) {
			/* the supply fell during the test: the outputs stay off */
			LOG_ERR("enable after the selftest refused (%d)", rc);
			(void)sstep_report_fault(&data->drv, (rc == SSTEP_ERR_VBUS)
								     ? SSTEP_FAULT_UNDERVOLTAGE
								     : SSTEP_FAULT_HAL);
		}
		return;
	}
	data->queued_op = QUEUED_NONE;
	LOG_ERR("selftest: %s, phase %c, %d mA at duty %u, legs sum up to %d counts (limit %d), "
		"%u periods in a row above (fails at %u)",
		selftest_str(st.selftest), (st.selftest_phase == 0U) ? 'A' : 'B',
		st.selftest_i_ma, st.selftest_duty, data->kirchhoff_peak, data->kirchhoff_limit,
		data->kirchhoff_run_max, data->kirchhoff_fail);
	/* Stepper-C has latched what it found; the timeout and the sensor of the
	 * shared leg are latched here.
	 */
	(void)sstep_report_fault(&data->drv, SSTEP_FAULT_SELFTEST);
}
#endif

static int drv_enable(const struct device *dev)
{
	const struct sstep_motor_child_config *child = dev->config;
	const struct sstep_motor_config *cfg = child->parent->config;
	struct sstep_motor_data *data = child->parent->data;
	struct power_stage_info info;
	int ret;

	CHECK_CALLER(data);

	ret = power_stage_get_info(cfg->stage, &info);
	if (ret != 0) {
		return ret;
	}
	if (!info.outputs_allowed) {
		LOG_ERR("enable refused: the power stage does not allow its outputs");
		return -EPERM;
	}

	struct sstep_status st;

	ret = to_errno(sstep_get_status(&data->drv, &st));
	if (ret != 0) {
		return ret;
	}
#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	if (data->enable_pending) {
		return 0; /* already accepted */
	}
	if ((st.state == SSTEP_STATE_DISABLED) &&
	    (!data->selftest_passed || IS_ENABLED(CONFIG_SSTEP_MOTOR_SELFTEST_EVERY_ENABLE))) {
		/* Accepted at once; sstep_motor_process() enables Stepper-C after the test */
		ret = start_selftest(cfg, data);
		if (ret == -ERANGE) {
			LOG_ERR("enable refused: VBUS %u mV outside %u..%u mV", st.vbus_mv,
				cfg->vbus_min_mv, cfg->vbus_max_mv);
		}
		return ret;
	}
#endif
	/* Bus voltage outside the limits: -ERANGE, not latched (the supply may come up) */
	ret = to_errno(sstep_enable(&data->drv));
	if (ret == -ERANGE) {
		LOG_ERR("enable refused: VBUS %u mV outside %u..%u mV", st.vbus_mv, cfg->vbus_min_mv,
			cfg->vbus_max_mv);
	}
	return ret;
}

static int drv_disable(const struct device *dev)
{
	const struct sstep_motor_child_config *child = dev->config;
	struct sstep_motor_data *data = child->parent->data;

	CHECK_CALLER(data);
#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
	if (data->enable_pending) {
		data->enable_pending = false; /* sstep_disable() stops the test */
		data->queued_op = QUEUED_NONE;
		(void)atomic_set(&data->selftest_active, 0);
	}
#endif
	return to_errno(sstep_disable(&data->drv));
}

static int drv_set_micro_step_res(const struct device *dev,
				  const enum stepper_micro_step_resolution res)
{
	const struct sstep_motor_child_config *child = dev->config;
	const struct sstep_motor_config *cfg = child->parent->config;

	return ((uint32_t)res == cfg->microsteps) ? 0 : -ENOTSUP;
}

static int drv_get_micro_step_res(const struct device *dev,
				  enum stepper_micro_step_resolution *res)
{
	const struct sstep_motor_child_config *child = dev->config;
	const struct sstep_motor_config *cfg = child->parent->config;

	*res = (enum stepper_micro_step_resolution)cfg->microsteps;
	return 0;
}

static int drv_set_event_cb(const struct device *dev, stepper_event_cb_t callback, void *user_data)
{
	const struct sstep_motor_child_config *child = dev->config;
	struct sstep_motor_data *data = child->parent->data;

	data->drv_cb = callback;
	data->drv_cb_data = user_data;
	data->drv_dev = dev;
	return 0;
}

static DEVICE_API(stepper, sstep_stepper_api) = {
	.enable = drv_enable,
	.disable = drv_disable,
	.set_micro_step_res = drv_set_micro_step_res,
	.get_micro_step_res = drv_get_micro_step_res,
	.set_event_cb = drv_set_event_cb,
};

/* ------------------------------------------------------------------------ */
/* Class "stepper_ctrl" on the child sfs,sstep-stepper-ctrl                 */

static struct sstep_motor_data *ctrl_data(const struct device *dev)
{
	const struct sstep_motor_child_config *child = dev->config;
	struct sstep_motor_data *data = child->parent->data;

	CHECK_CALLER(data);
	return data;
}

/* A rejected ramp changes nothing: what is stored is always what Stepper-C accepted */
static int set_limits(struct sstep_motor_data *data, uint32_t speed, uint32_t accel, uint32_t decel)
{
	const int rc = sstep_set_motion_limits(&data->drv, speed, accel, decel);

	if (rc == SSTEP_OK) {
		data->speed = speed;
		data->accel = accel;
		data->decel = decel;
	}
	return to_errno(rc);
}

static int ctrl_set_reference_position(const struct device *dev, const int32_t value)
{
	const struct sstep_motor_child_config *child = dev->config;
	const struct sstep_motor_config *cfg = child->parent->config;
	struct sstep_motor_data *data = ctrl_data(dev);
	struct sstep_status before;
	int rc;

	(void)sstep_get_status(&data->drv, &before);
	rc = sstep_set_position(&data->drv, value);
	if (rc == SSTEP_OK) {
		/* the rotor stays where it is, only its coordinate changes */
		(void)power_stage_position_shifted(cfg->stage, value - before.position);
	}
	return to_errno(rc);
}

static int ctrl_get_actual_position(const struct device *dev, int32_t *value)
{
	struct sstep_motor_data *data = ctrl_data(dev);
	struct sstep_status st;
	const int rc = sstep_get_status(&data->drv, &st);

	if (rc == SSTEP_OK) {
		*value = st.position;
	}
	return to_errno(rc);
}

static int ctrl_set_event_cb(const struct device *dev, stepper_ctrl_event_callback_t callback,
			     void *user_data)
{
	const struct sstep_motor_child_config *child = dev->config;
	struct sstep_motor_data *data = child->parent->data;

	data->ctrl_cb = callback;
	data->ctrl_cb_data = user_data;
	data->ctrl_dev = dev;
	return 0;
}

static int ctrl_set_microstep_interval(const struct device *dev,
				       const uint64_t microstep_interval_ns)
{
	struct sstep_motor_data *data = ctrl_data(dev);
	uint64_t speed;

	if (microstep_interval_ns == 0U) {
		return -EINVAL;
	}
	speed = (NSEC_PER_SEC + (microstep_interval_ns / 2U)) / microstep_interval_ns;
	if ((speed == 0U) || (speed > UINT32_MAX)) {
		return -EINVAL;
	}
	return set_limits(data, (uint32_t)speed, data->accel, data->decel);
}

static int ctrl_configure_ramp(const struct device *dev, const struct stepper_ctrl_ramp *ramp)
{
	struct sstep_motor_data *data = ctrl_data(dev);

	if ((ramp->speed_max == 0U) || (ramp->acceleration_max == 0U)) {
		return -EINVAL;
	}
	/* deceleration 0: same as the acceleration */
	return set_limits(data, ramp->speed_max, ramp->acceleration_max, ramp->deceleration_max);
}


#if defined(CONFIG_SSTEP_MOTOR_SELFTEST)
/* Motion while the self-test runs: kept and executed when it passes (the latest wins) */
static bool queue_motion(struct sstep_motor_data *data, enum queued_op op, int32_t arg)
{
	if (!data->enable_pending) {
		return false;
	}
	data->queued_op = op;
	data->queued_arg = arg;
	return true;
}
#else
#define queue_motion(data, op, arg) false
#endif

static int ctrl_move_by(const struct device *dev, const int32_t micro_steps)
{
	struct sstep_motor_data *data = ctrl_data(dev);

	if (queue_motion(data, QUEUED_MOVE_BY, micro_steps)) {
		return 0;
	}
	return to_errno(sstep_move_by(&data->drv, micro_steps));
}

static int ctrl_move_to(const struct device *dev, const int32_t micro_steps)
{
	struct sstep_motor_data *data = ctrl_data(dev);

	if (queue_motion(data, QUEUED_MOVE_TO, micro_steps)) {
		return 0;
	}
	return to_errno(sstep_move_to(&data->drv, micro_steps));
}

static int ctrl_run(const struct device *dev, const enum stepper_ctrl_direction direction)
{
	struct sstep_motor_data *data = ctrl_data(dev);
	const int32_t speed = (int32_t)MIN(data->speed, (uint32_t)INT32_MAX);
	const int32_t velocity = (direction == STEPPER_CTRL_DIRECTION_NEGATIVE) ? -speed : speed;

	if (queue_motion(data, QUEUED_RUN, velocity)) {
		return 0;
	}
	return to_errno(sstep_run(&data->drv, velocity));
}

static int ctrl_stop(const struct device *dev)
{
	struct sstep_motor_data *data = ctrl_data(dev);

	if (queue_motion(data, QUEUED_STOP, 0)) {
		return 0;
	}
	return to_errno(sstep_stop(&data->drv));
}

static int ctrl_is_moving(const struct device *dev, bool *is_moving)
{
	struct sstep_motor_data *data = ctrl_data(dev);
	struct sstep_status st;
	const int rc = sstep_get_status(&data->drv, &st);

	if (rc == SSTEP_OK) {
		*is_moving = st.state == SSTEP_STATE_MOVING;
	}
	return to_errno(rc);
}

static DEVICE_API(stepper_ctrl, sstep_stepper_ctrl_api) = {
	.set_reference_position = ctrl_set_reference_position,
	.get_actual_position = ctrl_get_actual_position,
	.set_event_cb = ctrl_set_event_cb,
	.set_microstep_interval = ctrl_set_microstep_interval,
	.configure_ramp = ctrl_configure_ramp,
	.move_by = ctrl_move_by,
	.move_to = ctrl_move_to,
	.run = ctrl_run,
	.stop = ctrl_stop,
	.is_moving = ctrl_is_moving,
};

/* ------------------------------------------------------------------------ */

static int build_config(const struct device *dev, struct sstep_config *out)
{
	const struct sstep_motor_config *cfg = dev->config;
	const struct sstep_motor_data *data = dev->data;
	const struct power_stage_info *ps = &data->stage_info;

	if ((ps->pwm_freq_hz % cfg->slow_loop_hz) != 0U) {
		LOG_ERR("PWM %u Hz is not a multiple of the slow loop %u Hz", ps->pwm_freq_hz,
			cfg->slow_loop_hz);
		return -EINVAL;
	}

	*out = (struct sstep_config){
		.motor = cfg->motor,
		.stage = {
			.topology = (ps->topology == POWER_STAGE_SHARED_LEG)
					    ? SSTEP_TOPOLOGY_SHARED_LEG
					    : SSTEP_TOPOLOGY_FULL_BRIDGE,
			.vbus_nominal_mv = cfg->vbus_nominal_mv,
			.vbus_min_mv = cfg->vbus_min_mv,
			.vbus_max_mv = cfg->vbus_max_mv,
			.i_full_scale_ma = ps->i_full_scale_ma,
			.i_trip_ma = cfg->i_trip_ma,
			.pwm_period_ticks = ps->pwm_period_ticks,
			.pwm_freq_hz = ps->pwm_freq_hz,
			.dead_time_ns = ps->dead_time_ns,
			.adc_i_zero = ps->adc_i_zero,
			.adc_i_span = ps->adc_i_span,
			.adc_vbus_mv_full = ps->adc_vbus_mv_full,
			.adc_full = ps->adc_full,
			.sample_window_ticks = ps->sample_window_ticks,
		},
		.ctrl = {
			.drive_mode = cfg->voltage_drive ? SSTEP_DRIVE_VOLTAGE
							 : SSTEP_DRIVE_CURRENT,
			/* one fast-loop call per PWM period */
			.fast_loop_hz = ps->pwm_freq_hz,
			.slow_loop_hz = cfg->slow_loop_hz,
			.microsteps = cfg->microsteps,
			.current_bw_hz = cfg->current_bw_hz,
			.i_run_ma = cfg->i_run_ma,
			.i_hold_ma = cfg->i_hold_ma,
			.hold_delay_ms = cfg->hold_delay_ms,
			.v_max = cfg->v_max,
			.accel = cfg->accel_max,
			.decel = 0U,
			.i_slew_ma_per_ms = cfg->i_slew_ma_per_ms,
		},
		.hal = {
			.context = (void *)dev,
			.pwm_write = hal_pwm_write,
			.output_enable = hal_output_enable,
			.on_fault = on_fault,
			.on_event = on_event,
		},
	};
	return 0;
}

static int sstep_motor_init(const struct device *dev)
{
	const struct sstep_motor_config *cfg = dev->config;
	struct sstep_motor_data *data = dev->data;
	struct sstep_config config;
	int ret;

	if (!device_is_ready(cfg->stage)) {
		LOG_ERR("%s not ready", cfg->stage->name);
		return -ENODEV;
	}
	ret = power_stage_get_info(cfg->stage, &data->stage_info);
	if (ret != 0) {
		return ret;
	}
	ret = build_config(dev, &config);
	if (ret != 0) {
		return ret;
	}
	ret = sstep_init(&data->drv, &config);
	if (ret != SSTEP_OK) {
		LOG_ERR("sstep_init: %d", ret);
		return to_errno(ret);
	}

	data->stage = cfg->stage;
	data->speed = cfg->v_max;
	data->accel = cfg->accel_max;
	data->decel = 0U;
	data->i_run_ma = cfg->i_run_ma;
	data->i_hold_ma = cfg->i_hold_ma;

	if (cfg->aux_trip) {
		/* The node asks for this protection: without it, do not run */
		ret = power_stage_set_aux_trip(cfg->stage, cfg->i_trip_ma);
		if (ret != 0) {
			LOG_ERR("aux-trip: %s cannot check it (%d)", cfg->stage->name, ret);
			return ret;
		}
	} else {
		(void)power_stage_set_aux_trip(cfg->stage, 0U);
	}
	ret = power_stage_set_fast_callback(cfg->stage, fast_loop, &data->drv);
	if (ret != 0) {
		return ret;
	}

	LOG_INF("Stepper-C %s: %u microsteps, run %u mA, hold %u mA, %s drive", SSTEP_VERSION,
		cfg->microsteps, cfg->i_run_ma, cfg->i_hold_ma,
		cfg->voltage_drive ? "voltage" : "current");
	return 0;
}

#define DT_DRV_COMPAT sfs_sstep_motor

#define SSTEP_MOTOR_DEFINE(n)                                                                      \
	static const struct sstep_motor_config sstep_motor_config_##n = {                          \
		.stage = DEVICE_DT_GET(DT_INST_PHANDLE(n, power_stage)),                           \
		.motor = {                                                                         \
			.r_mohm = DT_INST_PROP(n, phase_resistance_milliohm),                      \
			.l_uh = DT_INST_PROP(n, phase_inductance_microhenry),                      \
			.i_rated_ma = DT_INST_PROP(n, rated_current_milliamp),                     \
			.ke_uv_s_per_rad = DT_INST_PROP(n, back_emf_uv_s_per_rad),                 \
			.full_steps_per_rev = DT_INST_PROP(n, full_steps_per_rev),                 \
		},                                                                                 \
		.vbus_nominal_mv = DT_INST_PROP(n, vbus_nominal_mv),                               \
		.vbus_min_mv = DT_INST_PROP(n, vbus_min_mv),                                       \
		.vbus_max_mv = DT_INST_PROP(n, vbus_max_mv),                                       \
		.i_trip_ma = DT_INST_PROP(n, trip_current_milliamp),                               \
		.aux_trip = DT_INST_PROP(n, aux_trip),                                             \
		.voltage_drive = DT_INST_PROP(n, voltage_drive),                                   \
		.slow_loop_hz = DT_INST_PROP(n, slow_loop_hz),                                     \
		.microsteps = DT_INST_PROP(n, microsteps),                                         \
		.current_bw_hz = DT_INST_PROP(n, current_bandwidth_hz),                            \
		.i_run_ma = DT_INST_PROP(n, run_current_milliamp),                                 \
		.i_hold_ma = DT_INST_PROP(n, hold_current_milliamp),                               \
		.hold_delay_ms = DT_INST_PROP(n, hold_delay_ms),                                   \
		.v_max = DT_INST_PROP(n, max_velocity),                                            \
		.accel_max = DT_INST_PROP(n, max_acceleration),                                    \
		.i_slew_ma_per_ms = DT_INST_PROP(n, current_slew_ma_per_ms),                       \
		.i_selftest_ma = (DT_INST_PROP(n, selftest_current_milliamp) != 0)                 \
					 ? DT_INST_PROP(n, selftest_current_milliamp)               \
					 : DT_INST_PROP(n, rated_current_milliamp) * 3 / 10,        \
	};                                                                                         \
	static struct sstep_motor_data sstep_motor_data_##n;                                       \
	DEVICE_DT_INST_DEFINE(n, sstep_motor_init, NULL, &sstep_motor_data_##n,                    \
			      &sstep_motor_config_##n, POST_KERNEL,                                \
			      CONFIG_SSTEP_MOTOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(SSTEP_MOTOR_DEFINE)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT sfs_sstep_stepper_driver

static int child_init(const struct device *dev)
{
	const struct sstep_motor_child_config *child = dev->config;

	return device_is_ready(child->parent) ? 0 : -ENODEV;
}

#define SSTEP_STEPPER_DRIVER_DEFINE(n)                                                             \
	static const struct sstep_motor_child_config sstep_stepper_driver_config_##n = {           \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(n)),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, child_init, NULL, NULL, &sstep_stepper_driver_config_##n,         \
			      POST_KERNEL, CONFIG_SSTEP_MOTOR_CHILD_INIT_PRIORITY,                 \
			      &sstep_stepper_api);

DT_INST_FOREACH_STATUS_OKAY(SSTEP_STEPPER_DRIVER_DEFINE)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT sfs_sstep_stepper_ctrl

#define SSTEP_STEPPER_CTRL_DEFINE(n)                                                               \
	static const struct sstep_motor_child_config sstep_stepper_ctrl_config_##n = {             \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(n)),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, child_init, NULL, NULL, &sstep_stepper_ctrl_config_##n,           \
			      POST_KERNEL, CONFIG_SSTEP_MOTOR_CHILD_INIT_PRIORITY,                 \
			      &sstep_stepper_ctrl_api);

DT_INST_FOREACH_STATUS_OKAY(SSTEP_STEPPER_CTRL_DEFINE)
