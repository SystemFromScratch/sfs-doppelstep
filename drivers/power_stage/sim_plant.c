/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated motor (see sim_plant.h): Zephyr adapter of the plant model of
 * Stepper-Lab (sil/c/sstep_plant.c, the code of its Xcos plant block),
 * built in single precision with the averaged stage. Parameters: the motor and
 * stage files of Stepper-Lab (generated sstep_plant_params.h), each value
 * overridable with CONFIG_SFS_SIM_* (-1 = file value). Here the topology is the
 * shared leg of the ESC1 and outputs off means open windings (the currents are
 * zero, the rotor coasts). The load torque acts from the first time the
 * outputs are enabled: at power-on the joint rests on a support. See
 * docs/simulation.md.
 */

#include "sim_plant.h"

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>

#include <sstep_plant.h>
#include <sstep_plant_params.h>

/* The tests parse the log lines of this module, also those of app/src/sim_log.c */
LOG_MODULE_REGISTER(sim_plant, CONFIG_POWER_STAGE_LOG_LEVEL);

BUILD_ASSERT(SSTEP_PLANT_FLOAT, "the firmware builds the model in single precision");
BUILD_ASSERT(SSTEP_PLANT_PARAM_LEG_ERROR_N <= SSTEP_PLANT_LEG_ERROR_MAX,
	     "leg error table of the stage file too long for sstep_plant");

#define TWO_PI_F 6.28318531f

/* Kconfig override (integer, scaled to SI) or the value of the parameter file */
#define PARAM(sym, scale, file) \
	((CONFIG_##sym >= 0) ? (float)CONFIG_##sym * (scale) : (float)(file))

static struct sstep_plant plant;
static struct sstep_plant_adc adc;
static int32_t usteps_per_e_turn;

/* Fast-loop interrupt only */
static float cmp[SSTEP_PLANT_LEGS];

/* Written by threads, read by the interrupt (single aligned words) */
static volatile bool enabled;
static volatile bool loaded; /* outputs enabled once: the load acts */
static volatile float load;
static volatile int32_t pos_offset;

/* Published every period (sequence lock: odd while the interrupt writes) */
static struct sim_plant_state pub;
static volatile uint32_t pub_seq;

static void publish(void)
{
	const bool en = enabled;

	pub_seq++;
	barrier_dmem_fence_full();
	pub.i_a_ma = (int32_t)lroundf(plant.x[0] * 1000.0f);
	pub.i_b_ma = (int32_t)lroundf(plant.x[1] * 1000.0f);
	pub.v_a_mv = (int32_t)lroundf(plant.v[0] * 1000.0f);
	pub.v_b_mv = (int32_t)lroundf(plant.v[1] * 1000.0f);
	pub.speed_mrpm = (int32_t)lroundf(plant.x[2] * (60000.0f / TWO_PI_F));
	pub.rotor_usteps = plant.e_turns * usteps_per_e_turn +
			   (int32_t)lroundf(plant.x[3] * ((float)usteps_per_e_turn / TWO_PI_F)) +
			   pos_offset;
	pub.load_unm = (int32_t)lroundf(load * 1e6f);
	pub.enabled = en;
	barrier_dmem_fence_full();
	pub_seq++;
}

/* ------------------------------------------------------------------------ */

void sim_plant_sample(struct power_stage_sample *sample, uint32_t *i_w_ma)
{
	float counts[3];

	sstep_plant_adc_sample(&plant, &adc, counts);
	sample->i_a = (uint16_t)counts[0];
	sample->i_b = (uint16_t)counts[1];
	sample->vbus = (uint16_t)counts[2];
	*i_w_ma = (uint32_t)lroundf(fabsf(plant.x[0] + plant.x[1]) * 1000.0f);
}

void sim_plant_set_compare(const uint16_t *compare, size_t legs)
{
	for (size_t k = 0; k < MIN(legs, SSTEP_PLANT_LEGS); k++) {
		cmp[k] = (float)compare[k];
	}
}

void sim_plant_set_enabled(bool enable)
{
	enabled = enable;
	if (enable) {
		loaded = true;
	}
}

void sim_plant_step(void)
{
	sstep_plant_period(&plant, loaded ? load : 0.0f);
	/* voltage of the next period from the compare values just written */
	sstep_plant_set_compare(&plant, cmp, enabled ? 1 : 0);
	publish();
}

void sim_plant_set_load(int32_t load_unm)
{
	load = (float)load_unm * 1e-6f;
}

void sim_plant_set_vbus(uint32_t vbus_mv)
{
	/* single aligned word, read by the interrupt (ADC sample and legs) */
	plant.stage.vbus = (float)vbus_mv * 1e-3f;
}

void sim_plant_shift_position(int32_t delta_usteps)
{
	pos_offset += delta_usteps;
}

void sim_plant_get_state(struct sim_plant_state *state)
{
	uint32_t seq;

	do {
		seq = pub_seq;
		barrier_dmem_fence_full();
		*state = pub;
		barrier_dmem_fence_full();
	} while (((seq & 1U) != 0U) || (seq != pub_seq));
}

/* ------------------------------------------------------------------------ */

static bool off_by_more_than(float value, float ref, float rel)
{
	return fabsf(value - ref) > rel * fabsf(ref);
}

int sim_plant_init(const struct power_stage_info *info, uint32_t microsteps,
		   uint32_t full_steps_per_rev)
{
	static const float err_i[] = SSTEP_PLANT_PARAM_LEG_ERROR_I;
	static const float err_v[] = SSTEP_PLANT_PARAM_LEG_ERROR_V;
	const struct sstep_plant_motor motor = {
		.r = PARAM(SFS_SIM_R_MOHM, 1e-3f, SSTEP_PLANT_PARAM_R),
		.l = PARAM(SFS_SIM_L_UH, 1e-6f, SSTEP_PLANT_PARAM_L),
		.kt = PARAM(SFS_SIM_KT_UNM_PER_A, 1e-6f, SSTEP_PLANT_PARAM_KT),
		.ns = (float)SSTEP_PLANT_PARAM_NS,
		.j = PARAM(SFS_SIM_INERTIA_G_MM2, 1e-9f, SSTEP_PLANT_PARAM_J),
		.b = PARAM(SFS_SIM_VISCOUS_UNM_S, 1e-6f, SSTEP_PLANT_PARAM_B),
		.td = PARAM(SFS_SIM_DETENT_UNM, 1e-6f, SSTEP_PLANT_PARAM_TD),
		.tc = PARAM(SFS_SIM_COULOMB_UNM, 1e-6f, SSTEP_PLANT_PARAM_TC),
		.w_c = (float)SSTEP_PLANT_PARAM_W_C,
	};
	struct sstep_plant_stage stage = {
		.topology = SSTEP_PLANT_SHARED_LEG,
		.model = SSTEP_PLANT_STAGE_AVG,
		.off = SSTEP_PLANT_OFF_OPEN,
		.locked = IS_ENABLED(CONFIG_SFS_SIM_LOCKED_ROTOR) ? 1 : 0,
		.vbus = (float)CONFIG_SFS_SIM_VBUS_MV * 1e-3f,
		.fs = (float)info->pwm_freq_hz,
		.period = (float)info->pwm_period_ticks,
		.nsub = CONFIG_SFS_SIM_SUBSTEPS,
	};

	if ((info->i_full_scale_ma == 0U) || (info->adc_vbus_mv_full == 0U)) {
		return -EINVAL;
	}
	usteps_per_e_turn = (int32_t)(4U * microsteps);

	if (IS_ENABLED(CONFIG_SFS_SIM_LEG_ERROR)) {
		stage.leg_error_n = ARRAY_SIZE(err_i);
		for (size_t k = 0; k < ARRAY_SIZE(err_i); k++) {
			stage.leg_error_i[k] = err_i[k];
			stage.leg_error_v[k] = err_v[k];
		}
	}
	if (sstep_plant_init(&plant, &motor, &stage) != 0) {
		LOG_ERR("invalid model parameters");
		return -EINVAL;
	}

	adc = (struct sstep_plant_adc){
		.i_full = (float)info->i_full_scale_ma * 1e-3f,
		.span = (float)info->adc_i_span,
		.zero = (float)info->adc_i_zero,
		.v_full = (float)info->adc_vbus_mv_full * 1e-3f,
		.full = (float)info->adc_full,
	};
	for (size_t k = 0; k < SSTEP_PLANT_LEGS; k++) {
		cmp[k] = (float)info->pwm_period_ticks / 2.0f;
	}
	sim_plant_set_load(CONFIG_SFS_SIM_LOAD_UNM);
	publish();

	LOG_WRN("SIMULATED MOTOR: gate drivers stay off, the controller drives the model");
	LOG_INF("model %s: R %d mOhm, L %d uH, Kt %d uNm/A, J %d g*mm^2, Vbus %u mV, "
		"leg error %s, %u RK4 step(s)",
		SSTEP_PLANT_PARAM_MOTOR_NAME, (int)lroundf(motor.r * 1e3f),
		(int)lroundf(motor.l * 1e6f), (int)lroundf(motor.kt * 1e6f),
		(int)lroundf(motor.j * 1e9f), CONFIG_SFS_SIM_VBUS_MV,
		(stage.leg_error_n > 0U) ? "on" : "off", CONFIG_SFS_SIM_SUBSTEPS);
	if ((int)SSTEP_PLANT_PARAM_NS != (int)full_steps_per_rev) {
		LOG_WRN("model has %d full steps per revolution, the controller %u",
			(int)SSTEP_PLANT_PARAM_NS, full_steps_per_rev);
	}
	if ((stage.leg_error_n > 0U) &&
	    (off_by_more_than(stage.vbus, (float)SSTEP_PLANT_PARAM_STAGE_VBUS, 0.05f) ||
	     off_by_more_than(stage.fs, (float)SSTEP_PLANT_PARAM_STAGE_FS, 0.05f))) {
		LOG_WRN("leg error table measured at %d mV / %d Hz, model runs at %u mV / %u Hz",
			(int)(SSTEP_PLANT_PARAM_STAGE_VBUS * 1000), (int)SSTEP_PLANT_PARAM_STAGE_FS,
			CONFIG_SFS_SIM_VBUS_MV, info->pwm_freq_hz);
	}
	return 0;
}
