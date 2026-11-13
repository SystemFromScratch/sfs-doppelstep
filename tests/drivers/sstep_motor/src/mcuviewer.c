/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUViewer variables of the motor driver (scenario .mcuviewer): scaling of the
 * currents and the phase voltages, averages, peak-to-peak window and capture buffer.
 * The motor without aux trip fails at init, so the motor `motor` owns the probe.
 */

#include <stdlib.h>

#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/sstep_motor.h>

#include "fake_power_stage.h"
#include "sstep_mcuviewer.h"

static const struct device *const stage = DEVICE_DT_GET(DT_NODELABEL(stage));
static const struct device *const motor = DEVICE_DT_GET(DT_NODELABEL(motor));
static const struct device *const drv = DEVICE_DT_GET(DT_NODELABEL(motor_drv));

/* Scaling of the fake stage */
#define ADC_ZERO     2048
#define ADC_VBUS_24V 2866U
#define I_FS_MA      60155
#define I_SPAN       (-2048)
#define VBUS_FULL_MV 34283
#define ADC_FULL     4095
#define PERIOD       4250

static int32_t counts_to_ma(int32_t counts)
{
	return (int32_t)(((int64_t)counts * I_FS_MA) / I_SPAN);
}

static const int32_t vbus_mv = (int32_t)(((int64_t)ADC_VBUS_24V * VBUS_FULL_MV) / ADC_FULL);

static void fire(int32_t da, int32_t db)
{
	const struct power_stage_sample s = {
		.i_a = (uint16_t)(ADC_ZERO + da),
		.i_b = (uint16_t)(ADC_ZERO + db),
		.vbus = ADC_VBUS_24V,
		.i_aux = (uint16_t)(ADC_ZERO - da - db),
	};

	fake_power_stage_fire(stage, &s, 0U);
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)stepper_disable(drv);
	(void)sstep_motor_clear_faults(motor);
	fake_power_stage_set_outputs_allowed(stage, true);
	mcuv.capture_arm = 0U;
	fire(0, 0); /* ends a capture that a test left running */
	fire(0, 0); /* a bus voltage for the enable check */
	zassert_ok(sstep_motor_process(motor));
}

ZTEST(mcuviewer, test_header_describes_the_stage)
{
	zassert_equal(mcuv.magic, SSTEP_MCUV_MAGIC);
	zassert_equal(mcuv.version, SSTEP_MCUV_VERSION);
	zassert_equal(mcuv.pwm_freq_hz, 20000U);
	zassert_equal(mcuv.shared_leg, 1U);
	zassert_equal(mcuv.window_periods, 2000U, "100 ms");
	zassert_equal(mcuv.capture_length, CONFIG_SSTEP_MOTOR_MCUVIEWER_SAMPLES);
}

ZTEST(mcuviewer, test_currents_and_bus_voltage_are_in_engineering_units)
{
	const uint32_t periods = mcuv.periods;

	fire(-100, 40);
	zassert_equal(mcuv.periods, periods + 1U);
	zassert_within(mcuv.i_a_ma, counts_to_ma(-100), 1);
	zassert_within(mcuv.i_b_ma, counts_to_ma(40), 1);
	zassert_within(mcuv.i_w_ma, counts_to_ma(60), 1);
	zassert_within(mcuv.vbus_mv, vbus_mv, 1);
	zassert_true(mcuv.i_a_ma > 0, "-100 counts are +2.9 A with the negative span");
}

ZTEST(mcuviewer, test_phase_voltage_is_zero_with_the_outputs_off)
{
	fire(0, 0);
	zassert_equal(mcuv.outputs_on, 0U);
	zassert_equal(mcuv.v_a_mv, 0);
	zassert_equal(mcuv.v_b_mv, 0);
}

ZTEST(mcuviewer, test_phase_voltage_follows_the_compares_of_the_sampled_period)
{
	zassert_ok(stepper_enable(drv));
	for (int k = 0; k < 200; k++) {
		fire(0, 0); /* no current: the loop drives a voltage towards the hold current */
	}

	const uint16_t *cmp = fake_power_stage_compare(stage);
	const int32_t da = (int32_t)cmp[0] - (int32_t)cmp[2];
	const int32_t db = (int32_t)cmp[1] - (int32_t)cmp[2];

	fire(0, 0);
	zassert_equal(mcuv.outputs_on, 1U);
	zassert_within(mcuv.v_a_mv, (da * vbus_mv) / PERIOD, 2);
	zassert_within(mcuv.v_b_mv, (db * vbus_mv) / PERIOD, 2);
	zassert_true((abs(mcuv.v_a_mv) + abs(mcuv.v_b_mv)) > 1000, "the loop drives a voltage");
	zassert_ok(stepper_disable(drv));
}

ZTEST(mcuviewer, test_average_settles_on_a_constant_current)
{
	for (int k = 0; k < 4000; k++) {
		fire(-50, 0);
	}
	zassert_within(mcuv.i_a_avg_ma, counts_to_ma(-50), 2);
	zassert_within(mcuv.i_b_avg_ma, 0, 2);
}

ZTEST(mcuviewer, test_peak_to_peak_over_a_window)
{
	/* Two full windows: the second one only sees the toggle by one count */
	for (uint32_t k = 0U; k < 2U * mcuv.window_periods; k++) {
		fire(((k & 1U) != 0U) ? -1 : 0, 3);
	}
	zassert_within(mcuv.i_a_pp_ma, abs(counts_to_ma(1)), 1);
	zassert_equal(mcuv.i_b_pp_ma, 0);
}

ZTEST(mcuviewer, test_capture_records_every_nth_period_and_stops_when_full)
{
	const uint32_t n = mcuv.capture_length;

	mcuv.capture_divider = 2U;
	mcuv.capture_arm = 1U;
	for (uint32_t k = 0U; k < 2U * n + 10U; k++) {
		fire(-(int32_t)k, 0);
	}
	zassert_equal(mcuv.capture_arm, 0U, "the buffer is full");
	zassert_equal(mcuv.capture_count, n);
	zassert_equal(mcuv.capture_rate_hz, 10000U);
	zassert_within(mcuv_buf[0].i_a_ma, counts_to_ma(0), 1);
	zassert_within(mcuv_buf[1].i_a_ma, counts_to_ma(-2), 1);
	zassert_within(mcuv_buf[5].i_a_ma, counts_to_ma(-10), 1);
	mcuv.capture_divider = 1U;
}

ZTEST(mcuviewer, test_capture_saturates_to_16_bits)
{
	mcuv.capture_divider = 1U;
	mcuv.capture_arm = 1U;
	fire(-2000, 0); /* about 58.8 A: beyond int16 in mA */
	zassert_equal(mcuv_buf[0].i_a_ma, INT16_MAX);
	zassert_true(mcuv.i_a_ma > INT16_MAX, "the live value is not saturated");
	mcuv.capture_arm = 0U;
}

ZTEST_SUITE(mcuviewer, NULL, NULL, before_each, NULL, NULL);
