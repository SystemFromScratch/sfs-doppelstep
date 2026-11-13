/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Measurement variables and capture buffer of the fast loop for MCUViewer
 * (CONFIG_SSTEP_MOTOR_MCUVIEWER), see sstep_mcuviewer.h and docs/measurements.md.
 *
 * Runs in the fast-loop interrupt: no kernel calls, no divisions (scales are
 * precomputed at attach), a few tens of CPU cycles per period.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "sstep_mcuviewer.h"

/* First-order average over 2^AVG_SHIFT periods: 256 periods = 12.8 ms at 20 kHz */
#define AVG_SHIFT 8
#define LEGS_MAX  4U
#define SAMPLES   CONFIG_SSTEP_MOTOR_MCUVIEWER_SAMPLES

/* Global and volatile: the debugger reads and writes them while the firmware runs */
volatile struct sstep_mcuviewer mcuv;
volatile struct sstep_mcuv_point mcuv_buf[SAMPLES];

/* tools/mcuviewer/mcuv.py reads the fields by these offsets */
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, magic) == 0U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, shared_leg) == 16U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, periods) == 20U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, vbus_mv) == 44U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, i_a_avg_ma) == 52U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, window_periods) == 68U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, capture_arm) == 80U);
BUILD_ASSERT(offsetof(struct sstep_mcuviewer, capture_rate_hz) == 96U);
BUILD_ASSERT(sizeof(struct sstep_mcuviewer) == 100U);
BUILD_ASSERT(sizeof(struct sstep_mcuv_point) == 8U);

static bool attached;

/* Scaling, from the power stage info */
static int32_t i_zero;     /* ADC reading at 0 A */
static int64_t k_i_q16;    /* mA per count, Q16, signed */
static int64_t k_vbus_q16; /* mV per count, Q16 */
static int64_t k_v_q32;    /* 1 / period, Q32 */

/* Legs of the two phases: phase voltage = leg hi - leg lo */
static uint8_t leg_a_hi;
static uint8_t leg_a_lo;
static uint8_t leg_b_hi;
static uint8_t leg_b_lo;

/* Compare values written last: they apply to the period sampled next */
static uint16_t cmp[LEGS_MAX];

/* Averages, value * 2^AVG_SHIFT */
static int32_t acc_i_a;
static int32_t acc_i_b;
static int32_t acc_v_a;
static int32_t acc_v_b;

/* Peak-to-peak window */
static uint32_t win_n;
static int32_t min_a;
static int32_t max_a;
static int32_t min_b;
static int32_t max_b;

/* Capture */
static bool capturing;
static uint32_t cap_div;
static uint32_t cap_skip;
static uint32_t cap_n;

static int16_t sat16(int32_t v)
{
	return (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
}

static int32_t mul_shift(int64_t a, int64_t k, unsigned int shift)
{
	/* Arithmetic shift of a signed value (GCC and Clang) */
	return (int32_t)((a * k) >> shift);
}

bool sstep_mcuv_attach(const struct power_stage_info *info)
{
	if (attached || (info->adc_i_span == 0) || (info->adc_full == 0U) ||
	    (info->pwm_period_ticks == 0U) || (info->pwm_freq_hz == 0U)) {
		return false;
	}

	i_zero = (int32_t)info->adc_i_zero;
	k_i_q16 = ((int64_t)info->i_full_scale_ma << 16) / info->adc_i_span;
	k_vbus_q16 = ((int64_t)info->adc_vbus_mv_full << 16) / info->adc_full;
	k_v_q32 = ((int64_t)1 << 32) / info->pwm_period_ticks;

	const bool shared = (info->topology == POWER_STAGE_SHARED_LEG);

	/* Stepper-C: shared leg A = U - W, B = V - W; full bridge A = 0 - 1, B = 2 - 3 */
	leg_a_hi = 0U;
	leg_a_lo = shared ? 2U : 1U;
	leg_b_hi = shared ? 1U : 2U;
	leg_b_lo = shared ? 2U : 3U;

	mcuv.version = SSTEP_MCUV_VERSION;
	mcuv.pwm_freq_hz = info->pwm_freq_hz;
	mcuv.dead_time_ns = info->dead_time_ns;
	mcuv.shared_leg = shared ? 1U : 0U;
	mcuv.window_periods = MAX(info->pwm_freq_hz / 10U, 1U);
	mcuv.capture_divider = 1U;
	mcuv.capture_length = SAMPLES;
	mcuv.capture_rate_hz = info->pwm_freq_hz;
	mcuv.magic = SSTEP_MCUV_MAGIC;
	attached = true;
	return true;
}

void sstep_mcuv_compare(const uint16_t *compare, uint8_t legs)
{
	for (uint8_t k = 0U; k < MIN(legs, (uint8_t)LEGS_MAX); k++) {
		cmp[k] = compare[k];
	}
}

static void track_window(int32_t ia, int32_t ib)
{
	if (win_n == 0U) {
		min_a = ia;
		max_a = ia;
		min_b = ib;
		max_b = ib;
	} else {
		min_a = MIN(min_a, ia);
		max_a = MAX(max_a, ia);
		min_b = MIN(min_b, ib);
		max_b = MAX(max_b, ib);
	}
	win_n++;
	if (win_n >= mcuv.window_periods) {
		mcuv.i_a_pp_ma = max_a - min_a;
		mcuv.i_b_pp_ma = max_b - min_b;
		win_n = 0U;
	}
}

static void capture(int32_t ia, int32_t ib, int32_t va, int32_t vb)
{
	if (mcuv.capture_arm == 0U) {
		capturing = false;
		return;
	}
	if (!capturing) {
		capturing = true;
		cap_div = MAX(mcuv.capture_divider, 1U);
		cap_skip = 0U;
		cap_n = 0U;
		mcuv.capture_count = 0U;
		mcuv.capture_rate_hz = mcuv.pwm_freq_hz / cap_div;
	}
	if (cap_skip == 0U) {
		mcuv_buf[cap_n].i_a_ma = sat16(ia);
		mcuv_buf[cap_n].i_b_ma = sat16(ib);
		mcuv_buf[cap_n].v_a_mv = sat16(va);
		mcuv_buf[cap_n].v_b_mv = sat16(vb);
		cap_n++;
		mcuv.capture_count = cap_n;
		if (cap_n >= SAMPLES) {
			capturing = false;
			mcuv.capture_arm = 0U;
		}
	}
	cap_skip = (cap_skip + 1U >= cap_div) ? 0U : (cap_skip + 1U);
}

void sstep_mcuv_sample(const struct power_stage_sample *s, bool outputs_on)
{
	const int32_t ia = mul_shift((int32_t)s->i_a - i_zero, k_i_q16, 16U);
	const int32_t ib = mul_shift((int32_t)s->i_b - i_zero, k_i_q16, 16U);
	const int32_t iw = mul_shift((int32_t)s->i_aux - i_zero, k_i_q16, 16U);
	const int32_t vbus = mul_shift((int32_t)s->vbus, k_vbus_q16, 16U);
	int32_t va = 0;
	int32_t vb = 0;

	if (outputs_on) {
		va = mul_shift((int64_t)((int32_t)cmp[leg_a_hi] - (int32_t)cmp[leg_a_lo]) * vbus,
			       k_v_q32, 32U);
		vb = mul_shift((int64_t)((int32_t)cmp[leg_b_hi] - (int32_t)cmp[leg_b_lo]) * vbus,
			       k_v_q32, 32U);
	}

	mcuv.periods++;
	mcuv.i_a_ma = ia;
	mcuv.i_b_ma = ib;
	mcuv.i_w_ma = iw;
	mcuv.v_a_mv = va;
	mcuv.v_b_mv = vb;
	mcuv.vbus_mv = vbus;
	mcuv.outputs_on = outputs_on ? 1U : 0U;

	acc_i_a += ia - (acc_i_a >> AVG_SHIFT);
	acc_i_b += ib - (acc_i_b >> AVG_SHIFT);
	acc_v_a += va - (acc_v_a >> AVG_SHIFT);
	acc_v_b += vb - (acc_v_b >> AVG_SHIFT);
	mcuv.i_a_avg_ma = acc_i_a >> AVG_SHIFT;
	mcuv.i_b_avg_ma = acc_i_b >> AVG_SHIFT;
	mcuv.v_a_avg_mv = acc_v_a >> AVG_SHIFT;
	mcuv.v_b_avg_mv = acc_v_b >> AVG_SHIFT;

	track_window(ia, ib);
	capture(ia, ib, va, vb);
}
