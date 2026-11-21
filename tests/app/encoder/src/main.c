/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encoder tracking of the joint (app/src/enc_track.c): coordinates, alignment,
 * lost steps and the position regulator, without devices.
 */

#include <zephyr/ztest.h>

#include "enc_track.h"

#define UPR 3200 /* 200 full steps x 16 microsteps */

static const struct enc_track_cfg cfg = {
	.usteps_per_rev = UPR,
	.lost_threshold = 32,
	.settle_periods = 3,
	.correct_periods = 2,
	.correction_max = 16,
	.fault_reads = 3,
};

/* Raw reading of a position [microsteps] in [0, UPR) */
static uint16_t raw_of(int32_t usteps)
{
	const int32_t m = ((usteps % UPR) + UPR) % UPR;

	return (uint16_t)(((int64_t)m * ENC_COUNTS_PER_REV + UPR / 2) / UPR);
}

static struct enc_track t;

static void before_each(void *f)
{
	ARG_UNUSED(f);
	(void)enc_track_init(&t, &cfg, 0U);
}

/* Holding at the measured position until the alignment */
static void align(int32_t commanded)
{
	int32_t arg = 0;

	for (uint32_t i = 0U; i < cfg.settle_periods; i++) {
		(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, commanded, 0U, &arg);
	}
	zassert_true(t.aligned);
}

ZTEST(encoder, test_start_angle_is_signed)
{
	zassert_equal(enc_track_init(&t, &cfg, 0x4000U), 800, "a quarter turn");
	zassert_equal(enc_track_init(&t, &cfg, 0xC000U), -800, "three quarters = minus one");
	zassert_equal(enc_track_init(&t, &cfg, 0x8000U), -1600);
	zassert_equal(enc_track_init(&t, &cfg, 0U), 0);
	zassert_true(t.valid);
}

ZTEST(encoder, test_counts_round_to_the_nearest_microstep)
{
	enc_track_sample(&t, 10U); /* 0.49 microsteps */
	zassert_equal(t.position, 0);
	enc_track_sample(&t, 11U); /* 0.54 */
	zassert_equal(t.position, 1);
	enc_track_sample(&t, (uint16_t)-11); /* -0.54 */
	zassert_equal(t.position, -1);
}

ZTEST(encoder, test_turns_are_unwrapped_both_ways)
{
	/* three turns forward in steps of 1/8 turn */
	for (int32_t p = 400; p <= 3 * UPR; p += 400) {
		enc_track_sample(&t, raw_of(p));
	}
	zassert_equal(t.position, 3 * UPR);
	/* and five back */
	for (int32_t p = 3 * UPR - 400; p >= -2 * UPR; p -= 400) {
		enc_track_sample(&t, raw_of(p));
	}
	zassert_equal(t.position, -2 * UPR);
}

ZTEST(encoder, test_with_the_outputs_off_the_command_follows_the_shaft)
{
	int32_t arg = 0;

	enc_track_sample(&t, raw_of(123));
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_DISABLED, 0, 800U, &arg), ENC_FOLLOW);
	zassert_equal(arg, 123);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_DISABLED, 123, 800U, &arg), ENC_NONE);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_FAULT, 100, 800U, &arg), ENC_FOLLOW);
}

ZTEST(encoder, test_alignment_after_settling_at_standstill)
{
	int32_t arg = 0;

	/* the rotor snapped by 20 microsteps at the enable */
	enc_track_sample(&t, raw_of(20));
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg), ENC_NONE);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg), ENC_NONE);
	zassert_false(t.aligned, "not before settle_periods");
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg), ENC_FOLLOW);
	zassert_equal(arg, 20);
	zassert_true(t.aligned);
}

ZTEST(encoder, test_a_move_before_the_alignment_postpones_it)
{
	int32_t arg = 0;

	(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg);
	(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_MOVING, 50, 0U, &arg), ENC_NONE);
	zassert_equal(t.settle, 0U, "settling starts again at standstill");
	/* no lost-step check while not aligned, whatever the difference */
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_MOVING, 500, 0U, &arg), ENC_NONE);
	zassert_equal(t.lost, 0U);
}

ZTEST(encoder, test_lost_step_beyond_the_threshold)
{
	int32_t arg = 0;

	align(0);
	enc_track_sample(&t, raw_of(-32));
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_MOVING, 0, 0U, &arg), ENC_NONE,
		      "at the limit");
	enc_track_sample(&t, raw_of(-33));
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_MOVING, 0, 0U, &arg), ENC_LOST);
	zassert_equal(t.lost, 1U);
	zassert_false(t.aligned, "checks wait for the next alignment");
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_MOVING, 0, 0U, &arg), ENC_NONE, "once");
	zassert_equal(t.lost, 1U);
}

ZTEST(encoder, test_regulator_leads_the_field_until_the_goal_is_reached)
{
	int32_t arg = 0;
	const uint32_t gain = 781U; /* Position P Gain 800 */

	align(0);
	enc_track_set_goal(&t, 0);
	/* load angle: the rotor 10 microsteps behind the field at 0 */
	enc_track_sample(&t, raw_of(-10));
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, gain, &arg), ENC_NONE);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, gain, &arg), ENC_CORRECT);
	zassert_equal(arg, 8, "0.781 x 10, rounded");
	zassert_true(t.correcting);

	/* the field at 8, the rotor at -2 */
	enc_track_sample(&t, raw_of(-2));
	(void)enc_track_step(&t, SSTEP_MOTOR_MOVING, 8, gain, &arg);
	zassert_true(t.correcting, "still the correction");
	(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, 8, gain, &arg);
	zassert_false(t.correcting);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 8, gain, &arg), ENC_CORRECT);
	zassert_equal(arg, 10);

	/* on the goal: nothing more to do */
	enc_track_sample(&t, raw_of(0));
	(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, 10, gain, &arg);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 10, gain, &arg), ENC_NONE);
}

ZTEST(encoder, test_regulator_is_limited_and_can_be_off)
{
	int32_t arg = 0;

	align(0);
	enc_track_set_goal(&t, 0);
	enc_track_sample(&t, raw_of(-30)); /* more than the hold can take */
	for (int i = 0; i < 20; i++) {
		(void)enc_track_step(&t, SSTEP_MOTOR_HOLDING, t.goal + t.correction, 1000U, &arg);
	}
	zassert_equal(t.correction, 16, "correction_max");

	enc_track_set_goal(&t, 0);
	for (int i = 0; i < 10; i++) {
		zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 0U, &arg), ENC_NONE,
			      "gain 0: open loop");
	}
	enc_track_clear_goal(&t);
	for (int i = 0; i < 10; i++) {
		zassert_equal(enc_track_step(&t, SSTEP_MOTOR_HOLDING, 0, 1000U, &arg), ENC_NONE,
			      "no goal after a run or a stop");
	}
}

ZTEST(encoder, test_shift_moves_measured_and_goal_together)
{
	enc_track_sample(&t, raw_of(100));
	enc_track_set_goal(&t, 100);
	enc_track_shift(&t, -3200); /* Clear Multi-turn */
	zassert_equal(t.position, -3100);
	zassert_equal(t.goal, -3100);
	enc_track_sample(&t, raw_of(110));
	zassert_equal(t.position, -3090, "the offset stays");
}

ZTEST(encoder, test_failed_reads_make_a_latched_fault)
{
	int32_t arg = 0;

	enc_track_read_failed(&t);
	enc_track_read_failed(&t);
	zassert_false(t.fault);
	enc_track_sample(&t, 0U); /* a good read in between resets the count */
	enc_track_read_failed(&t);
	enc_track_read_failed(&t);
	zassert_false(t.fault);
	enc_track_read_failed(&t);
	zassert_true(t.fault);
	zassert_false(t.valid);
	zassert_equal(enc_track_step(&t, SSTEP_MOTOR_DISABLED, 99, 800U, &arg), ENC_NONE,
		      "no action from a failed encoder");
}

ZTEST_SUITE(encoder, NULL, NULL, before_each, NULL, NULL);
