/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The self-test and the bus voltage check of stepper_enable(), on the fake power
 * stage with a resistive load model fired by a timer (scenario .selftest).
 */

#include <errno.h>

#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/sstep_motor.h>

#include "fake_power_stage.h"

static const struct device *const stage = DEVICE_DT_GET(DT_NODELABEL(stage));
static const struct device *const motor = DEVICE_DT_GET(DT_NODELABEL(motor));
static const struct device *const drv = DEVICE_DT_GET(DT_NODELABEL(motor_drv));

#define PERIOD   4250
#define ZERO     2048
#define I_FS_MA  60155 /* scaling of the fake stage */
#define R_MOHM   1600
#define VBUS_24V 2866U
#define VBUS_8V  955U

/* Coil currents follow the voltage at once (no inductance, no dead time) */
struct load {
	bool open_a;
	bool dead_w_sensor; /* the shunt of the shared leg reads 0 A */
	uint16_t vbus;
	uint32_t periods_a; /* periods with coil A driven */
	uint32_t periods_b;
	bool outputs_seen;
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
	int32_t ia = 0;
	int32_t ib = 0;

	if (on) {
		l->outputs_seen = true;
		if (!l->open_a) {
			ia = ((int32_t)cmp[0] - cmp[2]) * vbus_mv / PERIOD * 1000 / R_MOHM;
		}
		ib = ((int32_t)cmp[1] - cmp[2]) * vbus_mv / PERIOD * 1000 / R_MOHM;
		l->periods_a += (cmp[0] > 0U) ? 1U : 0U;
		l->periods_b += (cmp[1] > 0U) ? 1U : 0U;
	}
	out->i_a = counts(ia);
	out->i_b = counts(ib);
	out->i_aux = l->dead_w_sensor ? counts(0) : counts(-(ia + ib));
	out->vbus = l->vbus;
}

static struct sstep_motor_status status(void)
{
	struct sstep_motor_status st;

	zassert_ok(sstep_motor_get_status(motor, &st));
	return st;
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

ZTEST(motor_selftest, test_enable_runs_the_selftest_once)
{
	zassert_ok(stepper_enable(drv));
	zassert_true(load.periods_a > 0U, "coil A tested");
	zassert_true(load.periods_b > 0U, "coil B tested");
	zassert_true(fake_power_stage_output_on(stage), "Stepper-C runs after the test");

	zassert_ok(stepper_disable(drv));
	load.periods_a = 0U;
	load.periods_b = 0U;
	zassert_ok(stepper_enable(drv));
	zassert_equal(load.periods_a + load.periods_b, 0U, "only at the first enable");
	zassert_equal(status().faults, 0U);
}

ZTEST(motor_selftest, test_failed_selftest_refuses_and_latches)
{
	load.open_a = true;
	zassert_equal(stepper_enable(drv), -EIO);
	zassert_false(fake_power_stage_output_on(stage));
	zassert_ok(sstep_motor_process(motor));
	zassert_equal(status().state, SSTEP_MOTOR_FAULT);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_SELFTEST) != 0U);

	/* Latched: no second test into a broken bridge */
	load.periods_a = 0U;
	zassert_not_equal(stepper_enable(drv), 0);
	zassert_equal(load.periods_a, 0U);
	zassert_false(fake_power_stage_output_on(stage));

	/* After clearing (a Reboot in the application) the test runs again */
	zassert_ok(sstep_motor_clear_faults(motor));
	load.open_a = false;
	zassert_ok(stepper_enable(drv));
	zassert_true(load.periods_a > 0U);
}

ZTEST(motor_selftest, test_broken_sensor_of_the_shared_leg_fails)
{
	/* Stepper-C sees iU and iV only; the driver adds up the three legs */
	load.dead_w_sensor = true;
	zassert_equal(stepper_enable(drv), -EIO);
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(status().state, SSTEP_MOTOR_FAULT);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_SELFTEST) != 0U);
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
