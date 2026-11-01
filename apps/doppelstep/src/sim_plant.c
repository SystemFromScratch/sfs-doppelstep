/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated motor (see sim_plant.h). Same equations as sstep_xcos_plant of
 * sfs-stepper-sim (sil/c/sstep_xcos.c), averaged power stage only, in single
 * precision for the Cortex-M4F FPU. State x = [iA iB w pe], pe = p theta:
 *
 *   L diA/dt = vA - R iA + Kt w sin(pe)                 p = Ns / 4
 *   L diB/dt = vB - R iB - Kt w cos(pe)
 *   J dw/dt  = Kt (-iA sin(pe) + iB cos(pe)) - Td sin(4 pe) - TL - B w - Tc tanh(w / wc)
 *   dpe/dt   = p w
 *
 * pe stays in [-pi, pi) and the electrical turns are counted apart, so the
 * float state keeps its precision however far the rotor turns. Shared leg:
 * vA = (dU - dW) Vbus, vB = (dV - dW) Vbus. With the outputs off the windings
 * are open: the currents are zero (the body diodes return the energy within
 * a fraction of a millisecond) and the rotor coasts.
 */

#include "sim_plant.h"

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>

#include "motor.h"

LOG_MODULE_REGISTER(sim_plant, CONFIG_SFS_LOG_LEVEL);

#define PI_F 3.14159265f
#define TWO_PI_F 6.28318531f
#define DRY_FRICTION_W_C 0.05f /* rad/s, Tc tanh(w / w_c): no stiction */

#define USTEPS_PER_E_TURN (4 * CONFIG_SFS_CTRL_MICROSTEPS)

struct motor {
	float r, l, kt, p, j, b, td, tc;
};

static struct motor m;
static float h;            /* RK4 step [s] */
static float vbus;         /* [V] */
static float duty_scale;   /* 1 / pwm_period_ticks */
static float counts_per_a; /* signed, adc_i_span / i_full_scale */
static float adc_zero;
static float adc_full;
static uint16_t vbus_counts;

/* Fast-loop interrupt only */
static float x[4];
static int32_t e_turns;
static float v_a;
static float v_b;
static uint16_t cmp[SSTEP_LEGS_MAX];

/* Written by threads, read by the interrupt (single aligned words) */
static volatile bool enabled;
static volatile float load;
static volatile int32_t pos_offset;

/* Published every period (sequence lock: odd while the interrupt writes) */
static struct sim_plant_state pub;
static volatile uint32_t pub_seq;

/* ------------------------------------------------------------------------ */

static void deriv(const float *s, float va, float vb, float tl, float *d)
{
	const float sn = sinf(s[3]);
	const float cs = cosf(s[3]);
	float te = m.kt * (-s[0] * sn + s[1] * cs);

	if (m.td != 0.0f) {
		te -= m.td * sinf(4.0f * s[3]);
	}
	float tf = m.b * s[2];

	if (m.tc != 0.0f) {
		tf += m.tc * tanhf(s[2] / DRY_FRICTION_W_C);
	}

	d[0] = (va - m.r * s[0] + m.kt * s[2] * sn) / m.l;
	d[1] = (vb - m.r * s[1] - m.kt * s[2] * cs) / m.l;
	if (IS_ENABLED(CONFIG_SFS_SIM_LOCKED_ROTOR)) {
		d[2] = 0.0f;
		d[3] = 0.0f;
	} else {
		d[2] = (te - tl - tf) / m.j;
		d[3] = m.p * s[2];
	}
}

static void rk4(float va, float vb, float tl)
{
	float k1[4], k2[4], k3[4], k4[4], y[4];

	deriv(x, va, vb, tl, k1);
	for (int i = 0; i < 4; i++) {
		y[i] = x[i] + 0.5f * h * k1[i];
	}
	deriv(y, va, vb, tl, k2);
	for (int i = 0; i < 4; i++) {
		y[i] = x[i] + 0.5f * h * k2[i];
	}
	deriv(y, va, vb, tl, k3);
	for (int i = 0; i < 4; i++) {
		y[i] = x[i] + h * k3[i];
	}
	deriv(y, va, vb, tl, k4);
	for (int i = 0; i < 4; i++) {
		x[i] += (h / 6.0f) * (k1[i] + 2.0f * k2[i] + 2.0f * k3[i] + k4[i]);
	}

	if (x[3] >= PI_F) {
		x[3] -= TWO_PI_F;
		e_turns++;
	} else if (x[3] < -PI_F) {
		x[3] += TWO_PI_F;
		e_turns--;
	} else {
		/* within one turn */
	}
}

static uint16_t current_counts(float i)
{
	const float c = adc_zero + i * counts_per_a + 0.5f;

	if (c < 0.0f) {
		return 0U;
	}
	return (c > adc_full) ? (uint16_t)adc_full : (uint16_t)c;
}

static void publish(void)
{
	const bool en = enabled;

	pub_seq++;
	barrier_dmem_fence_full();
	pub.i_a_ma = (int32_t)lroundf(x[0] * 1000.0f);
	pub.i_b_ma = (int32_t)lroundf(x[1] * 1000.0f);
	pub.v_a_mv = (int32_t)lroundf(v_a * 1000.0f);
	pub.v_b_mv = (int32_t)lroundf(v_b * 1000.0f);
	pub.speed_mrpm = (int32_t)lroundf(x[2] * (60000.0f / TWO_PI_F));
	pub.rotor_usteps = e_turns * USTEPS_PER_E_TURN +
			   (int32_t)lroundf(x[3] * ((float)USTEPS_PER_E_TURN / TWO_PI_F)) +
			   pos_offset;
	pub.load_unm = (int32_t)lroundf(load * 1e6f);
	pub.enabled = en;
	barrier_dmem_fence_full();
	pub_seq++;
}

/* ------------------------------------------------------------------------ */

void sim_plant_sample(struct sstep_sample *sample, uint32_t *i_w_ma)
{
	sample->i_a = current_counts(x[0]);
	sample->i_b = current_counts(x[1]);
	sample->vbus = vbus_counts;
	*i_w_ma = (uint32_t)lroundf(fabsf(x[0] + x[1]) * 1000.0f);
}

void sim_plant_set_compare(const uint16_t compare[SSTEP_LEGS_MAX])
{
	for (size_t k = 0; k < SSTEP_LEGS_MAX; k++) {
		cmp[k] = compare[k];
	}
}

void sim_plant_set_enabled(bool enable)
{
	enabled = enable;
}

void sim_plant_step(void)
{
	const float tl = load;

	for (int k = 0; k < CONFIG_SFS_SIM_SUBSTEPS; k++) {
		rk4(v_a, v_b, tl);
	}

	/* voltage of the next period from the compare values just written */
	if (enabled) {
		const float d_w = (float)cmp[2] * duty_scale;

		v_a = ((float)cmp[0] * duty_scale - d_w) * vbus;
		v_b = ((float)cmp[1] * duty_scale - d_w) * vbus;
	} else {
		v_a = 0.0f;
		v_b = 0.0f;
		x[0] = 0.0f;
		x[1] = 0.0f;
	}

	publish();
}

void sim_plant_set_load(int32_t load_unm)
{
	load = (float)load_unm * 1e-6f;
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

#if CONFIG_SFS_SIM_LOG_PERIOD_MS > 0

static void log_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(log_work, log_fn);

static void log_fn(struct k_work *work)
{
	struct sim_plant_state s;
	struct motor_snapshot snap;

	ARG_UNUSED(work);
	sim_plant_get_state(&s);
	motor_get_snapshot(&snap);

	/* rotor behind the command, in electrical degrees: above 90 the motor is
	 * close to losing steps, a multiple of 360 means 4 full steps lost
	 */
	const int32_t lag = snap.status.position - s.rotor_usteps;

	LOG_INF("cmd %d rotor %d usteps (lag %d deg el), %d rpm, iA %d iB %d mA, "
		"vA %d vB %d mV, load %d uNm",
		snap.status.position, s.rotor_usteps, lag * 90 / CONFIG_SFS_CTRL_MICROSTEPS,
		s.speed_mrpm / 1000, s.i_a_ma, s.i_b_ma, s.v_a_mv, s.v_b_mv, s.load_unm);

	if (!IS_ENABLED(CONFIG_ARCH_POSIX)) {
		/* native_sim: code takes no simulated time */
		const uint32_t cycles = power_stage_fast_cycles_max(true);

		LOG_INF("fast loop max %u cycles (%u us of %u)", cycles,
			(uint32_t)((uint64_t)cycles * 1000000U / sys_clock_hw_cycles_per_sec()),
			1000000U / power_stage_get_info()->pwm_freq_hz);
	}

	(void)k_work_reschedule(&log_work, K_MSEC(CONFIG_SFS_SIM_LOG_PERIOD_MS));
}

#endif

int sim_plant_init(const struct power_stage_info *info)
{
	if ((info->pwm_period_ticks == 0U) || (info->pwm_freq_hz == 0U) ||
	    (info->i_full_scale_ma == 0U) || (info->adc_vbus_mv_full == 0U)) {
		return -EINVAL;
	}

	m = (struct motor){
		.r = (float)CONFIG_SFS_SIM_R_MOHM * 1e-3f,
		.l = (float)CONFIG_SFS_SIM_L_UH * 1e-6f,
		.kt = (float)CONFIG_SFS_SIM_KT_UNM_PER_A * 1e-6f,
		.p = (float)CONFIG_SFS_MOTOR_FULL_STEPS_PER_REV / 4.0f,
		.j = (float)CONFIG_SFS_SIM_INERTIA_G_MM2 * 1e-9f,
		.b = (float)CONFIG_SFS_SIM_VISCOUS_UNM_S * 1e-6f,
		.td = (float)CONFIG_SFS_SIM_DETENT_UNM * 1e-6f,
		.tc = (float)CONFIG_SFS_SIM_COULOMB_UNM * 1e-6f,
	};
	h = 1.0f / ((float)info->pwm_freq_hz * (float)CONFIG_SFS_SIM_SUBSTEPS);
	vbus = (float)CONFIG_SFS_SIM_VBUS_MV * 1e-3f;
	duty_scale = 1.0f / (float)info->pwm_period_ticks;
	counts_per_a = (float)info->adc_i_span * 1000.0f / (float)info->i_full_scale_ma;
	adc_zero = (float)info->adc_i_zero;
	adc_full = (float)info->adc_full;
	vbus_counts = (uint16_t)MIN((uint64_t)CONFIG_SFS_SIM_VBUS_MV * info->adc_full /
					    info->adc_vbus_mv_full,
				    UINT16_MAX);
	sim_plant_set_load(CONFIG_SFS_SIM_LOAD_UNM);

	for (size_t k = 0; k < SSTEP_LEGS_MAX; k++) {
		cmp[k] = info->pwm_period_ticks / 2U;
	}
	publish();

	LOG_WRN("SIMULATED MOTOR: gate drivers stay off, Stepper-C drives the model");
	LOG_INF("model: R %u mOhm, L %u uH, Kt %u uNm/A, J %u g*mm^2, Vbus %u mV, %u RK4 step(s)",
		CONFIG_SFS_SIM_R_MOHM, CONFIG_SFS_SIM_L_UH, CONFIG_SFS_SIM_KT_UNM_PER_A,
		CONFIG_SFS_SIM_INERTIA_G_MM2, CONFIG_SFS_SIM_VBUS_MV, CONFIG_SFS_SIM_SUBSTEPS);

#if CONFIG_SFS_SIM_LOG_PERIOD_MS > 0
	(void)k_work_schedule(&log_work, K_MSEC(CONFIG_SFS_SIM_LOG_PERIOD_MS));
#endif
	return 0;
}
