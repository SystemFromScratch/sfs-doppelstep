/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See enc_track.h. Integer arithmetic only (no floating point above the backends).
 */

#include "enc_track.h"

#include <stdlib.h>

/* counts -> microsteps, rounded to the nearest (half away from zero) */
static int32_t to_usteps(const struct enc_track *t, int64_t counts)
{
	const int64_t num = counts * (int64_t)t->cfg.usteps_per_rev;
	const int64_t half = ENC_COUNTS_PER_REV / 2;

	return (int32_t)((num >= 0) ? ((num + half) / ENC_COUNTS_PER_REV)
				    : -((-num + half) / ENC_COUNTS_PER_REV));
}

static int32_t clamp32(int32_t v, int32_t lim)
{
	return (v > lim) ? lim : ((v < -lim) ? -lim : v);
}

int32_t enc_track_init(struct enc_track *t, const struct enc_track_cfg *cfg, uint16_t raw)
{
	*t = (struct enc_track){.cfg = *cfg};
	t->last_raw = raw;
	t->counts = (int16_t)raw; /* half a turn either way */
	t->position = to_usteps(t, t->counts);
	t->valid = true;
	return t->position;
}

void enc_track_sample(struct enc_track *t, uint16_t raw)
{
	/* less than half a turn per period: the shortest way round is the right one */
	t->counts += (int16_t)(uint16_t)(raw - t->last_raw);
	t->last_raw = raw;
	t->failed_reads = 0U;
	t->position = to_usteps(t, t->counts) + t->offset;
}

void enc_track_read_failed(struct enc_track *t)
{
	if (t->failed_reads < UINT8_MAX) {
		t->failed_reads++;
	}
	if (t->failed_reads >= t->cfg.fault_reads) {
		t->fault = true;
		t->valid = false;
		t->aligned = false;
		t->goal_valid = false;
	}
}

void enc_track_shift(struct enc_track *t, int32_t delta_usteps)
{
	t->offset += delta_usteps;
	t->position += delta_usteps;
	t->goal += delta_usteps;
}

void enc_track_set_goal(struct enc_track *t, int32_t goal_usteps)
{
	t->goal = goal_usteps;
	t->goal_valid = true;
	t->correction = 0;
	t->tick = 0U;
	t->correcting = false;
}

void enc_track_clear_goal(struct enc_track *t)
{
	t->goal_valid = false;
	t->correction = 0;
	t->correcting = false;
}

enum enc_action enc_track_step(struct enc_track *t, enum sstep_motor_state state, int32_t commanded,
			       uint32_t gain_milli, int32_t *arg)
{
	if (!t->valid) {
		return ENC_NONE;
	}

	if ((state == SSTEP_MOTOR_DISABLED) || (state == SSTEP_MOTOR_FAULT)) {
		t->aligned = false;
		t->settle = 0U;
		t->correcting = false;
		/* The commanded position follows the shaft (moved by hand, by gravity) */
		if (commanded != t->position) {
			*arg = t->position;
			return ENC_FOLLOW;
		}
		return ENC_NONE;
	}

	if (state != SSTEP_MOTOR_MOVING) {
		t->correcting = false;
	}

	if (!t->aligned) {
		if (state == SSTEP_MOTOR_MOVING) {
			t->settle = 0U; /* align at the next standstill */
			return ENC_NONE;
		}
		if (++t->settle < t->cfg.settle_periods) {
			return ENC_NONE;
		}
		t->aligned = true;
		t->tick = 0U;
		if (commanded != t->position) {
			*arg = t->position;
			return ENC_FOLLOW;
		}
		return ENC_NONE;
	}

	if (abs(commanded - t->position) > t->cfg.lost_threshold) {
		t->lost++;
		t->aligned = false;
		t->settle = 0U;
		t->correction = 0;
		t->correcting = false;
		return ENC_LOST;
	}

	if ((state != SSTEP_MOTOR_HOLDING) || !t->goal_valid || (gain_milli == 0U)) {
		t->tick = 0U;
		return ENC_NONE;
	}
	if (++t->tick < t->cfg.correct_periods) {
		return ENC_NONE;
	}
	t->tick = 0U;

	const int32_t error = t->goal - t->position;
	const int32_t step =
		(int32_t)(((int64_t)error * (int64_t)gain_milli + ((error >= 0) ? 500 : -500)) /
			  1000);

	t->correction = clamp32(t->correction + step, t->cfg.correction_max);
	if ((t->goal + t->correction) != commanded) {
		*arg = t->goal + t->correction;
		t->correcting = true;
		return ENC_CORRECT;
	}
	return ENC_NONE;
}
