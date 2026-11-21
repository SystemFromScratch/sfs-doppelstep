/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encoder of the joint (chosen sfs,encoder): the measured rotor position in the
 * coordinate of the motor driver, the check for lost steps and the position
 * regulator. Pure logic without devices, called by the control task once per slow-loop
 * period; unit-tested on the host (tests/app/encoder).
 *
 * Coordinates: the encoder gives 65536 counts per motor revolution. The measured
 * position [microsteps] is counts * usteps_per_rev / 65536 + offset, unwrapped over any
 * number of turns. At the start the offset is 0 and the angle is taken as signed (half a
 * turn either way), so the motor driver starts at the absolute angle of the shaft.
 * Afterwards the measured and the commanded position share one coordinate: a shift of
 * the motor coordinate (Clear Multi-turn, reference position) shifts the offset too.
 *
 * Alignment: when the outputs come on, the rotor snaps to the field of the motor driver
 * (up to two full steps: the electrical angle of a stepper repeats every four). After
 * settle_periods of holding, the commanded position is set to the measured one (the
 * field does not move, only its coordinate) and the checks start. While the outputs are
 * off, the commanded position follows the shaft, so that a Goal Position derived from
 * Present Position after the next Torque Enable means no jump.
 *
 * Lost steps: with the checks armed, |commanded - measured| above lost_threshold means
 * the rotor has slipped to another equilibrium of the field (beyond two full steps the
 * torque turns round). Reported once per event; the checks wait for the next alignment.
 *
 * Regulator: a stepper holds its field position stiffly, less the load angle (up to a
 * full step at the run current). At standstill after a position move the regulator
 * leads the field by a correction until the measured position reaches the goal:
 * every correct_periods, correction += gain * (goal - measured), limited to
 * correction_max. gain_milli = 0 switches it off (open loop as without an encoder).
 */

#ifndef DOPPELSTEP_APP_ENC_TRACK_H_
#define DOPPELSTEP_APP_ENC_TRACK_H_

#include <stdbool.h>
#include <stdint.h>

#include <doppelstep/drivers/sstep_motor.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENC_COUNTS_PER_REV 65536

struct enc_track_cfg {
	uint32_t usteps_per_rev;  /**< Motor full steps per revolution * microsteps. */
	int32_t lost_threshold;   /**< |commanded - measured| of a lost step [microsteps]. */
	uint32_t settle_periods;  /**< Holding periods after the enable before the alignment. */
	uint32_t correct_periods; /**< Periods between two corrections of the regulator. */
	int32_t correction_max;   /**< Largest correction of the regulator [microsteps]. */
	uint8_t fault_reads;      /**< Failed reads in a row that make an encoder fault. */
};

/** What the control task does with the motor driver after enc_track_step(). */
enum enc_action {
	ENC_NONE,
	ENC_FOLLOW,  /**< Set the reference position to arg: the rotor does not move. */
	ENC_CORRECT, /**< Move to arg: a correction of the regulator. */
	ENC_LOST,    /**< A lost step: report it (the counter in the state has grown). */
};

struct enc_track {
	struct enc_track_cfg cfg;
	bool valid;           /**< At least one reading, and no encoder fault. */
	bool fault;           /**< fault_reads failed reads in a row; latched. */
	bool aligned;         /**< Outputs on, settled: lost-step check and regulator armed. */
	bool goal_valid;      /**< A position goal for the regulator. */
	bool correcting;      /**< The running move is a correction of the regulator. */
	uint8_t failed_reads; /**< In a row. */
	uint16_t last_raw;    /**< Last reading [counts]. */
	int64_t counts;       /**< Unwrapped since the start [counts]. */
	int32_t offset;       /**< Measured = usteps(counts) + offset [microsteps]. */
	int32_t position;     /**< Measured position [microsteps]. */
	int32_t goal;         /**< Position goal of the regulator [microsteps]. */
	int32_t correction;   /**< Lead of the field over the goal [microsteps]. */
	uint32_t settle;      /**< Holding periods since the enable. */
	uint32_t tick;        /**< Periods since the last correction. */
	uint32_t lost;        /**< Lost steps since the start. */
};

/**
 * First reading: the signed angle of the shaft becomes the measured position.
 * @return the measured position [microsteps], the start reference of the motor driver.
 */
int32_t enc_track_init(struct enc_track *t, const struct enc_track_cfg *cfg, uint16_t raw);

/** A reading [counts]; updates the measured position. */
void enc_track_sample(struct enc_track *t, uint16_t raw);

/** A failed reading; after cfg.fault_reads in a row the encoder is in fault (latched). */
void enc_track_read_failed(struct enc_track *t);

/** The motor coordinate moved by @p delta_usteps (reference position): so does the measured. */
void enc_track_shift(struct enc_track *t, int32_t delta_usteps);

/** A position move to @p goal_usteps from the host: the new goal of the regulator. */
void enc_track_set_goal(struct enc_track *t, int32_t goal_usteps);

/** A run, a stop or a disable: no position goal any more. */
void enc_track_clear_goal(struct enc_track *t);

/**
 * Once per period, after the slow loop.
 * @param state      State of the motor driver.
 * @param commanded  Commanded position of the motor driver [microsteps].
 * @param gain_milli Regulator gain x1000 (Position P Gain * 1000 / 1024), 0 = off.
 * @param arg        Argument of the action.
 */
enum enc_action enc_track_step(struct enc_track *t, enum sstep_motor_state state, int32_t commanded,
			       uint32_t gain_milli, int32_t *arg);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_APP_ENC_TRACK_H_ */
