/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * API contract of the power_stage class on the simulation backend.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/power_stage.h>
#include <doppelstep/drivers/power_stage_sim.h>

static const struct device *const sim = DEVICE_DT_GET(DT_NODELABEL(sim_stage));
static const struct device *const minimal = DEVICE_DT_GET(DT_NODELABEL(fake_minimal));
static const struct device *const full = DEVICE_DT_GET(DT_NODELABEL(fake_full));

static volatile uint32_t calls;
static volatile bool aux_seen;
static volatile uint16_t last_i_a;
static volatile uint16_t last_vbus;

static void count_cb(const struct power_stage_sample *s, bool aux_trip, void *ctx)
{
	ARG_UNUSED(ctx);
	calls++;
	last_i_a = s->i_a;
	last_vbus = s->vbus;
	if (aux_trip) {
		aux_seen = true;
	}
}

static void after_each(void *fixture)
{
	static const uint16_t centre[3] = {2125, 2125, 2125};

	ARG_UNUSED(fixture);
	(void)power_stage_output_enable(sim, false);
	(void)power_stage_write(sim, centre, ARRAY_SIZE(centre));
	(void)power_stage_set_fast_callback(sim, NULL, NULL);
	(void)power_stage_set_aux_trip(sim, 0U);
	calls = 0U;
	aux_seen = false;
}

ZTEST(power_stage, test_devices_are_ready)
{
	zassert_true(device_is_ready(sim));
	zassert_true(device_is_ready(minimal));
	zassert_true(device_is_ready(full));
}

ZTEST(power_stage, test_info_has_the_scaling_of_the_node)
{
	struct power_stage_info info;

	zassert_ok(power_stage_get_info(sim, &info));
	zassert_equal(info.topology, POWER_STAGE_SHARED_LEG);
	zassert_equal(info.legs, 3);
	zassert_true(info.outputs_allowed);
	zassert_equal(info.pwm_freq_hz, 20000U);
	zassert_equal(info.pwm_period_ticks, 4250);
	zassert_equal(info.i_full_scale_ma, 60155U);
	zassert_equal(info.adc_i_span, -2048);
	zassert_equal(info.adc_i_zero, 2048);
	zassert_equal(info.adc_vbus_mv_full, 34283U);
	zassert_equal(info.adc_full, 4095);
}

ZTEST(power_stage, test_fast_callback_runs_20_times_per_tick)
{
	zassert_ok(power_stage_set_fast_callback(sim, count_cb, NULL));
	k_msleep(100);
	zassert_within(calls, 2000U, 40U, "%u calls", calls);
}

ZTEST(power_stage, test_callback_null_stops_the_calls)
{
	uint32_t seen;

	zassert_ok(power_stage_set_fast_callback(sim, count_cb, NULL));
	k_msleep(5);
	zassert_ok(power_stage_set_fast_callback(sim, NULL, NULL));
	seen = calls;
	zassert_true(seen > 0U);
	k_msleep(5);
	zassert_equal(calls, seen);
}

ZTEST(power_stage, test_calibrate_rejects_zero_samples)
{
	zassert_equal(power_stage_calibrate(sim, 0U), -EINVAL);
	zassert_ok(power_stage_calibrate(sim, 16U));
}

ZTEST(power_stage, test_outputs_off_gives_zero_current_and_the_bus_voltage)
{
	zassert_ok(power_stage_set_fast_callback(sim, count_cb, NULL));
	k_msleep(5);
	zassert_within(last_i_a, 2048, 1, "i_a %u", last_i_a);
	/* 24 V of the model on a 34.283 V full scale */
	zassert_within(last_vbus, 2866, 2, "vbus %u", last_vbus);
}

/* Runs before the other calibrate test: the suite runs in alphabetical order */
ZTEST(power_stage, test_aux_trip_waits_for_calibration_then_follows_the_shared_leg)
{
	/* U and V high against W: both phases push their current through W */
	static const uint16_t drive[3] = {3000, 3000, 1250};

	zassert_ok(power_stage_set_fast_callback(sim, count_cb, NULL));
	zassert_ok(power_stage_set_aux_trip(sim, 100U));
	zassert_ok(power_stage_write(sim, drive, ARRAY_SIZE(drive)));
	zassert_ok(power_stage_output_enable(sim, true));
	k_msleep(50);
	zassert_false(aux_seen, "the sensor offsets are not known yet");

	zassert_equal(power_stage_calibrate(sim, 0U), -EINVAL);
	k_msleep(5);
	zassert_false(aux_seen, "a refused calibration does not count");

	zassert_ok(power_stage_calibrate(sim, 16U));
	k_msleep(5);
	zassert_true(aux_seen);
}

ZTEST(power_stage, test_model_state_follows_the_outputs)
{
	struct sim_plant_state state;

	zassert_ok(power_stage_sim_get_state(sim, &state));
	zassert_false(state.enabled);
	zassert_ok(power_stage_output_enable(sim, true));
	k_msleep(2);
	zassert_ok(power_stage_sim_get_state(sim, &state));
	zassert_true(state.enabled);
}

ZTEST(power_stage, test_simulation_follows_a_redefined_position)
{
	struct sim_plant_state before;
	struct sim_plant_state after;

	zassert_ok(power_stage_sim_get_state(sim, &before));
	zassert_ok(power_stage_position_shifted(sim, 800));
	k_msleep(2);
	zassert_ok(power_stage_sim_get_state(sim, &after));
	/* the rotor may still coast after an earlier test: 2 ms of that is small */
	zassert_within(after.rotor_usteps - before.rotor_usteps, 800, 64);
	zassert_ok(power_stage_position_shifted(sim, -800));
}

ZTEST(power_stage, test_optional_ops_return_enosys)
{
	struct power_stage_stats stats;

	zassert_equal(power_stage_get_stats(minimal, &stats), -ENOSYS);
	zassert_equal(power_stage_position_shifted(minimal, 1), -ENOSYS);
	zassert_equal(power_stage_set_aux_trip(minimal, 1U), -ENOSYS);
	zassert_equal(power_stage_get_stats(full, &stats), -ENOSYS);
	zassert_ok(power_stage_position_shifted(full, 1));
}

ZTEST(power_stage, test_stats_count_the_fast_loop_periods)
{
	struct power_stage_stats before;
	struct power_stage_stats after;

	zassert_ok(power_stage_get_stats(sim, &before));
	k_msleep(10);
	zassert_ok(power_stage_get_stats(sim, &after));
	zassert_within(after.isr_count - before.isr_count, 200U, 40U);
}

ZTEST_SUITE(power_stage, NULL, NULL, NULL, after_each, NULL);
