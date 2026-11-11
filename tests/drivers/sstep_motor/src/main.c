/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Motor driver through the standard stepper classes, on a fake power stage: the
 * test fires the fast loop itself, so every step is deterministic.
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
static const struct device *const motor_no_aux = DEVICE_DT_GET(DT_NODELABEL(motor_no_aux));

/* 0 A on both phases at 24 V; 200 counts are above the trip level of 119 counts */
#define ADC_ZERO     2048U
#define ADC_VBUS_24V 2866U
#define ADC_OVER     (ADC_ZERO - 200U)

static const struct power_stage_sample zero = {ADC_ZERO, ADC_ZERO, ADC_VBUS_24V};

static uint32_t steps_completed_events;
static uint32_t fault_events;
static uint32_t stall_events;

static void ctrl_event(const struct device *dev, const enum stepper_ctrl_event event, void *ud)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ud);
	if (event == STEPPER_CTRL_EVENT_STEPS_COMPLETED) {
		steps_completed_events++;
	}
}

static void drv_event(const struct device *dev, const enum stepper_event event, void *ud)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ud);
	if (event == STEPPER_EVENT_FAULT_DETECTED) {
		fault_events++;
	} else if (event == STEPPER_EVENT_STALL_DETECTED) {
		stall_events++;
	}
}

/* n slow-loop periods with their 20 fast-loop periods */
static void run_ms(uint32_t n)
{
	for (uint32_t k = 0U; k < n; k++) {
		for (uint32_t j = 0U; j < 20U; j++) {
			fake_power_stage_fire(stage, &zero, 0U);
		}
		zassert_ok(sstep_motor_process(motor));
	}
}

static struct sstep_motor_status status(void)
{
	struct sstep_motor_status st;

	zassert_ok(sstep_motor_get_status(motor, &st));
	return st;
}

static void before_each(void *fixture)
{
	const struct stepper_ctrl_ramp ramp = {
		.speed_max = 16000U,
		.acceleration_max = 64000U,
		.deceleration_max = 0U,
	};

	ARG_UNUSED(fixture);
	(void)stepper_disable(drv);
	(void)sstep_motor_clear_faults(motor);
	fake_power_stage_set_outputs_allowed(stage, true);
	zassert_ok(stepper_ctrl_configure_ramp(ctrl, &ramp));
	zassert_ok(stepper_ctrl_set_reference_position(ctrl, 0));
	zassert_ok(stepper_set_event_cb(drv, drv_event, NULL));
	zassert_ok(stepper_ctrl_set_event_cb(ctrl, ctrl_event, NULL));
	zassert_ok(sstep_motor_process(motor));
	steps_completed_events = 0U;
	fault_events = 0U;
	stall_events = 0U;
}

ZTEST(motor, test_devices_are_ready)
{
	zassert_true(device_is_ready(motor));
	zassert_true(device_is_ready(drv));
	zassert_true(device_is_ready(ctrl));
}

ZTEST(motor, test_aux_trip_on_a_stage_without_it_fails_at_init)
{
	/* The node asks for this overcurrent protection: losing it silently is not an option */
	zassert_false(device_is_ready(motor_no_aux));
}

ZTEST(motor, test_aux_trip_level_is_set_from_the_node)
{
	zassert_equal(fake_power_stage_aux_trip_ma(stage), 3500U);
}

ZTEST(motor, test_enable_is_refused_while_outputs_are_not_allowed)
{
	fake_power_stage_set_outputs_allowed(stage, false);
	zassert_equal(stepper_enable(drv), -EPERM);
	zassert_false(fake_power_stage_output_on(stage));
	run_ms(1);
	zassert_equal(status().state, SSTEP_MOTOR_DISABLED);
	zassert_equal(status().faults, 0U);
}

ZTEST(motor, test_enable_turns_outputs_on_and_holds)
{
	zassert_ok(stepper_enable(drv));
	zassert_true(fake_power_stage_output_on(stage));
	run_ms(1);
	zassert_equal(status().state, SSTEP_MOTOR_HOLDING);
	zassert_ok(stepper_disable(drv));
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(status().state, SSTEP_MOTOR_DISABLED);
}

ZTEST(motor, test_move_reaches_the_target_and_reports_once)
{
	bool moving;
	int32_t pos;

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 3200));
	run_ms(1);
	zassert_ok(stepper_ctrl_is_moving(ctrl, &moving));
	zassert_true(moving);
	zassert_equal(status().target, 3200);
	run_ms(500);
	zassert_ok(stepper_ctrl_is_moving(ctrl, &moving));
	zassert_false(moving);
	zassert_ok(stepper_ctrl_get_actual_position(ctrl, &pos));
	zassert_equal(pos, 3200);
	zassert_equal(steps_completed_events, 1U);
}

ZTEST(motor, test_move_by_is_relative)
{
	int32_t pos;

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 800));
	run_ms(300);
	zassert_ok(stepper_ctrl_move_by(ctrl, -300));
	run_ms(300);
	zassert_ok(stepper_ctrl_get_actual_position(ctrl, &pos));
	zassert_equal(pos, 500);
}

ZTEST(motor, test_ramp_limits_the_speed_of_a_move)
{
	const struct stepper_ctrl_ramp slow = {
		.speed_max = 1000U,
		.acceleration_max = 64000U,
		.deceleration_max = 0U,
	};

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_configure_ramp(ctrl, &slow));
	zassert_ok(stepper_ctrl_move_to(ctrl, 3200));
	run_ms(1000);
	zassert_equal(status().velocity, 1000);
	zassert_within(status().position, 1000, 40);
}

ZTEST(motor, test_run_uses_the_ramp_speed_and_stop_ends_it)
{
	const struct stepper_ctrl_ramp ramp = {
		.speed_max = 3200U,
		.acceleration_max = 64000U,
		.deceleration_max = 0U,
	};
	bool moving;

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_configure_ramp(ctrl, &ramp));
	zassert_ok(stepper_ctrl_run(ctrl, STEPPER_CTRL_DIRECTION_NEGATIVE));
	run_ms(100);
	zassert_equal(status().velocity, -3200);
	zassert_ok(stepper_ctrl_stop(ctrl));
	run_ms(100);
	zassert_equal(status().velocity, 0);
	zassert_ok(stepper_ctrl_is_moving(ctrl, &moving));
	zassert_false(moving);
}

ZTEST(motor, test_microstep_interval_sets_the_speed_of_a_run)
{
	zassert_equal(stepper_ctrl_set_microstep_interval(ctrl, 0U), -EINVAL);
	zassert_ok(stepper_ctrl_set_microstep_interval(ctrl, 500000U)); /* 2000 microsteps/s */
	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_run(ctrl, STEPPER_CTRL_DIRECTION_POSITIVE));
	run_ms(100);
	zassert_equal(status().velocity, 2000);
}

ZTEST(motor, test_zero_ramp_values_are_rejected)
{
	const struct stepper_ctrl_ramp no_speed = {.speed_max = 0U, .acceleration_max = 1000U};
	const struct stepper_ctrl_ramp no_accel = {.speed_max = 1000U, .acceleration_max = 0U};

	zassert_equal(stepper_ctrl_configure_ramp(ctrl, &no_speed), -EINVAL);
	zassert_equal(stepper_ctrl_configure_ramp(ctrl, &no_accel), -EINVAL);
}

ZTEST(motor, test_rejected_ramp_keeps_the_previous_one_and_stop_works)
{
	/* An acceleration below 8 microsteps/s^2 is below the resolution at 1 kHz */
	const struct stepper_ctrl_ramp too_low = {.speed_max = 1U, .acceleration_max = 1U};
	int32_t pos;

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 3200));
	run_ms(10);
	zassert_equal(stepper_ctrl_configure_ramp(ctrl, &too_low), -EINVAL);

	zassert_ok(stepper_ctrl_stop(ctrl));
	run_ms(500);
	zassert_equal(status().velocity, 0);
	zassert_ok(stepper_ctrl_get_actual_position(ctrl, &pos));
	zassert_true(pos < 3200, "the move must have been stopped, position %d", pos);
}

ZTEST(motor, test_stop_works_after_a_rejected_interval)
{
	int32_t pos;

	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 3200));
	run_ms(10);
	zassert_equal(stepper_ctrl_set_microstep_interval(ctrl, UINT64_MAX), -EINVAL);

	zassert_ok(stepper_ctrl_stop(ctrl));
	run_ms(500);
	zassert_ok(stepper_ctrl_get_actual_position(ctrl, &pos));
	zassert_true(pos < 3200, "the move must have been stopped, position %d", pos);
}

ZTEST(motor, test_microstep_resolution_is_fixed)
{
	enum stepper_micro_step_resolution res;

	zassert_ok(stepper_set_micro_step_res(drv, STEPPER_MICRO_STEP_16));
	zassert_equal(stepper_set_micro_step_res(drv, STEPPER_MICRO_STEP_32), -ENOTSUP);
	zassert_ok(stepper_get_micro_step_res(drv, &res));
	zassert_equal(res, STEPPER_MICRO_STEP_16);
}

ZTEST(motor, test_overcurrent_stops_outputs_inside_the_fast_loop)
{
	const struct power_stage_sample over = {ADC_OVER, ADC_ZERO, ADC_VBUS_24V};
	struct sstep_motor_status st;

	zassert_ok(stepper_enable(drv));
	run_ms(1);
	fake_power_stage_fire(stage, &over, 0U);
	zassert_false(fake_power_stage_output_on(stage));
	zassert_equal(fault_events, 0U, "events must not come from the interrupt context");

	zassert_ok(sstep_motor_process(motor));
	zassert_equal(fault_events, 1U);
	st = status();
	zassert_equal(st.state, SSTEP_MOTOR_FAULT);
	zassert_true((st.faults & SSTEP_MOTOR_FAULT_OVERCURRENT) != 0U);

	zassert_ok(sstep_motor_process(motor));
	zassert_equal(fault_events, 1U, "one event per new fault");
}

ZTEST(motor, test_aux_trip_reports_external_fault)
{
	zassert_ok(stepper_enable(drv));
	run_ms(1);
	fake_power_stage_fire(stage, &zero, POWER_STAGE_TRIP_AUX);
	zassert_false(fake_power_stage_output_on(stage));
	zassert_ok(sstep_motor_process(motor));
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_EXTERNAL) != 0U);
}

ZTEST(motor, test_fast_loop_overrun_reports_a_hal_fault)
{
	zassert_ok(stepper_enable(drv));
	run_ms(1);
	zassert_true(fake_power_stage_output_on(stage));
	fake_power_stage_fire(stage, &zero, POWER_STAGE_TRIP_OVERRUN);
	zassert_false(fake_power_stage_output_on(stage), "the fault keeps the outputs off");
	zassert_ok(sstep_motor_process(motor));
	zassert_equal(status().state, SSTEP_MOTOR_FAULT);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_HAL) != 0U);
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_EXTERNAL) == 0U,
		     "an overrun is not an overcurrent");
}

/* A flash write of the EEPROM area (allowed only with the torque off) stalls the CPU
 * for many PWM periods: harmless with the outputs off, so no fault may latch.
 */
ZTEST(motor, test_fast_loop_overrun_with_the_outputs_off_is_ignored)
{
	zassert_false(fake_power_stage_output_on(stage));
	fake_power_stage_fire(stage, &zero, POWER_STAGE_TRIP_OVERRUN);
	zassert_ok(sstep_motor_process(motor));
	zassert_equal(status().faults, 0U);
	zassert_ok(stepper_enable(drv), "Torque Enable still works");
	run_ms(1);
	zassert_true(fake_power_stage_output_on(stage));
}

ZTEST(motor, test_undervoltage_is_reported)
{
	const struct power_stage_sample low = {ADC_ZERO, ADC_ZERO, 1000U}; /* 8.4 V */

	zassert_ok(stepper_enable(drv));
	run_ms(1);
	fake_power_stage_fire(stage, &low, 0U);
	zassert_ok(sstep_motor_process(motor));
	zassert_true((status().faults & SSTEP_MOTOR_FAULT_UNDERVOLTAGE) != 0U);
}

ZTEST(motor, test_enable_in_a_fault_is_rejected_until_cleared)
{
	const struct power_stage_sample over = {ADC_OVER, ADC_ZERO, ADC_VBUS_24V};

	zassert_ok(stepper_enable(drv));
	run_ms(1);
	fake_power_stage_fire(stage, &over, 0U);
	zassert_ok(sstep_motor_process(motor));
	zassert_equal(stepper_enable(drv), -EBUSY);

	zassert_ok(sstep_motor_clear_faults(motor));
	zassert_equal(status().state, SSTEP_MOTOR_DISABLED);
	zassert_equal(status().faults, 0U);
	zassert_ok(stepper_enable(drv));

	/* a second fault after clearing is a new event */
	fake_power_stage_fire(stage, &over, 0U);
	zassert_ok(sstep_motor_process(motor));
	zassert_equal(fault_events, 2U);
}

ZTEST(motor, test_reference_position_is_rejected_while_moving)
{
	zassert_ok(stepper_enable(drv));
	zassert_ok(stepper_ctrl_move_to(ctrl, 3200));
	run_ms(10);
	zassert_equal(stepper_ctrl_set_reference_position(ctrl, 0), -EBUSY);
}

ZTEST(motor, test_reference_position_notifies_the_power_stage)
{
	const int32_t before = fake_power_stage_shift_total(stage);
	int32_t pos;

	zassert_ok(stepper_ctrl_set_reference_position(ctrl, 700));
	zassert_equal(fake_power_stage_shift_total(stage) - before, 700);
	zassert_ok(stepper_ctrl_get_actual_position(ctrl, &pos));
	zassert_equal(pos, 700);
}

ZTEST(motor, test_set_current_is_limited_to_the_node_values)
{
	struct sstep_motor_status st;

	zassert_ok(sstep_motor_set_current(motor, 5000U, 5000U));
	st = status();
	zassert_equal(st.i_run_ma, 1500U);
	zassert_equal(st.i_hold_ma, 500U);

	zassert_ok(sstep_motor_set_current(motor, 300U, 500U));
	st = status();
	zassert_equal(st.i_run_ma, 300U);
	zassert_equal(st.i_hold_ma, 300U, "hold never above run");

	zassert_ok(sstep_motor_set_current(motor, 1500U, 500U));
}

ZTEST(motor, test_status_amplitude_is_the_root_of_both_phases)
{
	/* 10 and 14 counts of 2048 at 60155 mA: 294 mA and 411 mA */
	const struct power_stage_sample s = {ADC_ZERO - 10U, ADC_ZERO - 14U, ADC_VBUS_24V};

	fake_power_stage_fire(stage, &s, 0U);
	zassert_within(status().i_amplitude_ma, 505U, 10U, "%u mA", status().i_amplitude_ma);
}

ZTEST(motor, test_status_reports_the_bus_voltage)
{
	fake_power_stage_fire(stage, &zero, 0U);
	zassert_within(status().vbus_mv, 24000U, 20U);
}

ZTEST_SUITE(motor, NULL, NULL, before_each, NULL, NULL);
