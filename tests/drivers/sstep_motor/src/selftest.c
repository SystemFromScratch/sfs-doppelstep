/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The self-test and the bus voltage check of stepper_enable(), on the fake power
 * stage with a resistive load model fired by a timer (scenario .selftest).
 * stepper_enable() starts the test and returns; the slow loop ends it, as the
 * control task does every millisecond.
 */

#include <errno.h>

#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/sstep_motor.h>

#include "fake_power_stage.h"

static const struct device *const stage = DEVICE_DT_GET(DT_NODELABEL(stage));
static const struct device *const motor = DEVICE_DT_GET(DT_NODELABEL(motor));
static const struct device *const drv = DEVICE_DT_GET(DT_NODELABEL(motor_drv));
static const struct device *const ctrl = DEVICE_DT_GET(DT_NODELABEL(motor_ctrl));

#define PERIOD   4250
#define ZERO     2048
#define I_FS_MA  60155 /* scaling of the fake stage */
#define R_MOHM   1600
#define VBUS_24V 2866U
#define VBUS_8V  955U

/* Coil currents follow the coil voltage with the time constant of the winding,
 * L / R = 3 mH / 1.6 Ohm, about 38 PWM periods (first order, no dead time): the self-test
 * and, after it, the current loop of Stepper-C act on it as on the motor.
 */
#define TAU_PERIODS 38
struct load {
	bool open_a;
	bool dead_w_sensor; /* the shunt of the shared leg reads 0 A */
	bool w_spike;       /* one sample of the shared leg 80 counts off (a switching edge) */
	uint32_t samples;
	uint16_t vbus;
	uint32_t periods_a; /* periods with coil A driven */
	uint32_t periods_b;
	bool outputs_seen;
	int32_t ia_ma; /* coil currents of the model */
	int32_t ib_ma;
};

static struct load load;

static uint16_t counts(int32_t i_ma)
{
	return (uint16_t)(ZERO + i_ma * -2048 / I_FS_MA);
}

static void plant(const uint16_t *cmp, bool on, struct power_stage_sample *out, void *ctx)
{
	struct load *l = ctx;
	const int32_t vbus_mv = l->vbus * 34283 / 4095;
	int32_t ia_ss = 0; /* steady-state currents of this voltage */
	int32_t ib_ss = 0;
	int32_t ia;
	int32_t ib;

	if (on) {
		l->outputs_seen = true;
		if (!l->open_a) {
			ia_ss = ((int32_t)cmp[0] - cmp[2]) * vbus_mv / PERIOD * 1000 / R_MOHM;
		}
		ib_ss = ((int32_t)cmp[1] - cmp[2]) * vbus_mv / PERIOD * 1000 / R_MOHM;
		l->periods_a += (cmp[0] > 0U) ? 1U : 0U;
		l->periods_b += (cmp[1] > 0U) ? 1U : 0U;
		l->ia_ma += (ia_ss - l->ia_ma) / TAU_PERIODS;
		l->ib_ma += (ib_ss - l->ib_ma) / TAU_PERIODS;
	} else {
		l->ia_ma = 0; /* outputs off: the current decays within a period */
		l->ib_ma = 0;
	}
	if (l->open_a) {
		l->ia_ma = 0;
	}
	ia = l->ia_ma;
	ib = l->ib_ma;
	out->i_a = counts(ia);
	out->i_b = counts(ib);
	out->i_aux = l->dead_w_sensor ? counts(0) : counts(-(ia + ib));
	l->samples++;
	if (l->w_spike && (l->samples == 400U)) {
		out->i_aux -= 80U;
	}
	out->vbus = l->vbus;
}

static struct sstep_motor_status status(void)
{
	struct sstep_motor_status st;

	zassert_ok(sstep_motor_get_status(motor, &st));
	return st;
}

/* 150 slow-loop periods: more than the self-test timeout (100) */
static void settle(void)
{
	for (int ms = 0; ms < 150; ms++) {
		(void)sstep_motor_process(motor);
		k_sleep(K_MSEC(1));
	}
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_power_stage_set_plant(stage, NULL, NULL);
	(void)stepper_disable(drv);
	(void)sstep_motor_clear_faults(motor);
	fake_power_stage_set_outputs_allowed(stage, true);
	load = (struct load){.vbus = VBUS_24V};
	fake_power_stage_set_plant(stage, plant, &load);
	k_sleep(K_MSEC(5)); /* a bus voltage reading */
}

static void after_each(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)stepper_disable(drv);
	fake_power_stage_set_plant(stage, NULL, NULL);
}

ZTEST(motor_selftest, test_enable_answers_before_the_selftest_ends)
{
	/* A DYNAMIXEL host waits about 34 ms for the answer to Torque Enable */
	zassert_ok(stepper_enable(drv));
	zassert_equal(load.periods_b, 0U, "returned before coil B was tested");
	zassert_equal(status().state, SSTEP_MOTOR_HOLDING, "torque on while the test runs");
	zassert_ok(stepper_enable(drv), "a second enable meanwhile is accepted");

	settle();
	zassert_true(load.periods_a > 0U, "coil A tested");
	zassert_true(load.periods_b > 0U, "coil B tested");
	zassert_true(fake_power_stage_output_on(stage), "Stepper-C runs after the test");
	zassert_equal(status().state, SSTEP_MOTOR_HOLDING);
	zassert_equal(status().faults, 0U);
}

ZTEST(motor_selftest, test_enable_runs_the_selftest_once)
{
	zassert_ok(stepper_enable(drv));
	settle();
	zassert_ok(stepper_disable(drv));
	load = (struct load){.vbus = VBUS_24V};
	zassert_ok(stepper_enable(drv));
	zassert_equal(load.periods_a + load.periods_b, 0U, "only at the first enable");
	zassert_true(fake_power_stage_output_on(stage));
	zassert_equal(status().faults, 0U);
}

ZTEST(motor_selftest, test_motion_during_the_selftest_runs_after_it)
{
	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 1000), "accepted while the test runs");
	zassert_equal(status().target, status().position, "not started yet");
	settle();
	zassert_equal(status().target, 1000, "executed after the test passed");
	zassert_equal(status().faults, 0U);
}

ZTEST(motor_selftest, test_disable_during_the_selftest_stops_it)
{
	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_disable(drv));
	load.periods_b = 0U;
	settle();
	zassert_equal(load.periods_b, 0U, "the test does not go on");
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(status().state, SSTEP_MOTOR_DISABLED);
	zassert_equal(status().faults, 0U, "not a fault");
}

ZTEST(motor_selftest, test_failed_selftest_latches_a_fault)
{
	load.open_a = true;
	zassert_ok(stepper_enable(drv), "accepted, as an XM430 accepts Torque Enable");
	settle();
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(status().state, SSTEP_MOTOR_FAULT);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_SELFTEST) != 0U);

	/* Latched: no second test into a broken bridge */
	load.periods_a = 0U;
	zassert_not_equal(stepper_enable(drv), 0);
	settle();
	zassert_equal(load.periods_a, 0U);
	zassert_false(fake_power_stage_output_on(stage));

	/* After clearing (a Reboot in the application) the test runs again */
	zassert_ok(sstep_motor_clear_faults(motor));
	load = (struct load){.vbus = VBUS_24V};
	zassert_ok(stepper_enable(drv));
	settle();
	zassert_true(load.periods_a > 0U);
	zassert_true(fake_power_stage_output_on(stage));
}

ZTEST(motor_selftest, test_broken_sensor_of_the_shared_leg_fails)
{
	/* Stepper-C sees iU and iV only; the driver adds up the three legs */
	load.dead_w_sensor = true;
	zassert_ok(stepper_enable(drv));
	settle();
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(status().state, SSTEP_MOTOR_FAULT);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_SELFTEST) != 0U);
}

ZTEST(motor_selftest, test_one_disturbed_sample_of_the_shared_leg_passes)
{
	/* a broken sensor is wrong as long as current flows; one disturbed sample is not */
	load.w_spike = true;
	zassert_ok(stepper_enable(drv));
	settle();
	zassert_true(load.samples > 400U, "the disturbed sample came during the test");
	zassert_true(fake_power_stage_output_on(stage));
	zassert_equal(status().faults, 0U);
}

ZTEST(motor_selftest, test_enable_with_vbus_out_of_range_is_refused)
{
	load.vbus = VBUS_8V;
	k_sleep(K_MSEC(5));
	load.outputs_seen = false;
	zassert_equal(stepper_enable(drv), -ERANGE);
	zassert_false(load.outputs_seen, "nothing energised before the voltage is checked");
	zassert_equal(status().faults, 0U, "not latched");
}

ZTEST_SUITE(motor_selftest, NULL, NULL, before_each, after_each, NULL);
