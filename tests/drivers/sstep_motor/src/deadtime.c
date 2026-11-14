/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dead-time compensation from the devicetree to Stepper-C (scenario .deadtime): two motors
 * in voltage drive hold 1.5 A in phase A, one with dead-time-comp-ns = 1050 and
 * dead-time-comp-band-milliamp = 250. The compensation adds the loss of both legs of
 * phase A, 2 t f P = 2 x 1050 ns x 20 kHz x 4250 = 178.5 compare ticks, and of the shared
 * leg to phase B, 89 ticks.
 */

#include <stdlib.h>

#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/sstep_motor.h>

#include "fake_power_stage.h"

static const struct device *const stage_vd = DEVICE_DT_GET(DT_NODELABEL(stage_vd));
static const struct device *const stage_comp = DEVICE_DT_GET(DT_NODELABEL(stage_vd_comp));
static const struct device *const motor_vd = DEVICE_DT_GET(DT_NODELABEL(motor_vd));
static const struct device *const motor_comp = DEVICE_DT_GET(DT_NODELABEL(motor_vd_comp));
static const struct device *const drv_vd = DEVICE_DT_GET(DT_NODELABEL(motor_vd_drv));
static const struct device *const drv_comp = DEVICE_DT_GET(DT_NODELABEL(motor_vd_comp_drv));

/* 0 A on both phases at 24 V */
static const struct power_stage_sample zero = {2048U, 2048U, 2866U, 2048U};

static void hold(const struct device *stage, const struct device *motor,
		 const struct device *drv)
{
	fake_power_stage_set_outputs_allowed(stage, true);
	fake_power_stage_fire(stage, &zero, 0U); /* a bus voltage for the enable check */
	zassert_ok(sstep_motor_process(motor));
	zassert_ok(stepper_enable(drv));
	for (int ms = 0; ms < 5; ms++) {
		for (int k = 0; k < 20; k++) {
			fake_power_stage_fire(stage, &zero, 0U);
		}
		zassert_ok(sstep_motor_process(motor));
	}
}

static int diff(const uint16_t *c, int hi, int lo)
{
	return (int)c[hi] - (int)c[lo];
}

ZTEST(deadtime, test_compensation_from_the_devicetree_reaches_the_library)
{
	hold(stage_vd, motor_vd, drv_vd);
	hold(stage_comp, motor_comp, drv_comp);

	const uint16_t *off = fake_power_stage_compare(stage_vd);
	const uint16_t *on = fake_power_stage_compare(stage_comp);
	const int da = diff(on, 0, 2) - diff(off, 0, 2);
	const int db = diff(on, 1, 2) - diff(off, 1, 2);

	TC_PRINT("phase A +%d ticks, phase B +%d ticks\n", da, db);
	zassert_within(da, 178, 2, "phase A: loss of legs U and W");
	zassert_within(db, 89, 2, "phase B: loss of leg W");
}

ZTEST_SUITE(deadtime, NULL, NULL, NULL, NULL, NULL);
