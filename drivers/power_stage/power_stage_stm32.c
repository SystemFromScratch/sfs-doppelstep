/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32 backend of the power_stage class: three half-bridges on an advanced-control
 * timer with complementary outputs, OPAMP current amplifiers, two ADCs with injected
 * conversions triggered by the timer. Hardware description: the node with compatible
 * "sfs,stm32-shared-leg-power-stage".
 *
 * Timing (center-aligned PWM, counter 0 -> ARR -> 0, one period = 2 * ARR ticks):
 *
 *   CNT  0 ........ ARR-1  ARR  ARR-1 ........ 0
 *   OC4REF          _______________                (PWM mode 2, CCR4 = ARR - 1)
 *   TRGO2 ---------^  injected conversions of ADC1 (U, VBUS, W) and ADC2 (V)
 *
 * At the top of the counter all legs are on their low-side switch (PWM mode 1:
 * high side while CNT < CCRx), the only moment the low-side shunts carry the
 * phase currents. The end of the ADC1 injected sequence runs the fast loop.
 *
 * Optional hardware trips (hw-trip-current-milliamp, hw-trip-vbus-mv): comparators
 * of the Zephyr comparator driver watch the OPAMP inputs and the bus divider against
 * DAC levels of the Zephyr DAC driver (the negative input of each comparator node
 * names its DAC channel); they drive the break inputs BRK and BRK2 of the timer,
 * which clear MOE in hardware. Timer, ADCs and break routing: stm32_pwm_adc.h.
 */

#define DT_DRV_COMPAT sfs_stm32_shared_leg_power_stage

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/opamp.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <doppelstep/drivers/power_stage.h>

#include "hw_trip.h"
#include "ntc.h"
#include "power_stage_scaling.h"
#include "stm32_pwm_adc.h"

LOG_MODULE_REGISTER(power_stage, CONFIG_POWER_STAGE_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one instance: fixed timer and ADCs");

#define PS_NODE DT_DRV_INST(0)

#define TIM_NODE DT_PHANDLE(PS_NODE, timer)
#define ADCM_NODE DT_PHANDLE(PS_NODE, adc_master)
#define ADCS_NODE DT_PHANDLE(PS_NODE, adc_slave)
#define ADC_COMMON_NODE DT_PARENT(ADCM_NODE)

/* LL programs these peripherals directly: no Zephyr driver may own them. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(TIM_NODE, okay), "power stage timer must be disabled");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(ADCM_NODE, okay), "power stage adc-master must be disabled");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(ADCS_NODE, okay), "power stage adc-slave must be disabled");
BUILD_ASSERT(DT_SAME_NODE(ADC_COMMON_NODE, DT_PARENT(ADCS_NODE)),
	     "adc-master and adc-slave must share one ADC common block");
BUILD_ASSERT(DT_PROP_LEN(PS_NODE, opamps) == 3, "three OPAMPs expected (U, V, W)");
BUILD_ASSERT(DT_PROP_LEN(PS_NODE, vbus_divider) == 2, "vbus-divider = <R_low R_high>");

#define ADC_BITS 12U
#define ADC_FULL ((1U << ADC_BITS) - 1U)
#define ADC_MID (1U << (ADC_BITS - 1U))

#define TIMx ((TIM_TypeDef *)DT_REG_ADDR(TIM_NODE))
#define ADC_M ((ADC_TypeDef *)DT_REG_ADDR(ADCM_NODE))
#define ADC_S ((ADC_TypeDef *)DT_REG_ADDR(ADCS_NODE))

#define CH_U __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, phase_u_channel))
#define CH_V __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, phase_v_channel))
#define CH_W __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, phase_w_channel))
#define CH_VBUS __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, vbus_channel))

/* Optional NTC (temperature-channel): fourth injected rank of the master ADC */
#define HAS_NTC DT_NODE_HAS_PROP(PS_NODE, temperature_channel)
#if HAS_NTC
BUILD_ASSERT(DT_NODE_HAS_PROP(PS_NODE, ntc_r25_ohm) && DT_NODE_HAS_PROP(PS_NODE, ntc_beta) &&
		     DT_NODE_HAS_PROP(PS_NODE, ntc_bottom_ohm),
	     "temperature-channel needs ntc-r25-ohm, ntc-beta and ntc-bottom-ohm");
#define CH_TEMP __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, temperature_channel))
#endif

/* Optional hardware trips and offset window (hw_trip.h) */
#define HAS_HW_OC DT_NODE_HAS_PROP(PS_NODE, hw_trip_current_milliamp)
#define HAS_HW_OV DT_NODE_HAS_PROP(PS_NODE, hw_trip_vbus_mv)
#define HAS_OFFSET_WINDOW DT_NODE_HAS_PROP(PS_NODE, current_offset_min_counts)
BUILD_ASSERT(HAS_OFFSET_WINDOW == DT_NODE_HAS_PROP(PS_NODE, current_offset_max_counts),
	     "current-offset-min-counts and current-offset-max-counts go together");
BUILD_ASSERT(HAS_HW_OC == DT_NODE_HAS_PROP(PS_NODE, hw_trip_current_comparators),
	     "hw-trip-current-milliamp and hw-trip-current-comparators go together");
BUILD_ASSERT(HAS_HW_OV == DT_NODE_HAS_PROP(PS_NODE, hw_trip_vbus_comparator),
	     "hw-trip-vbus-mv and hw-trip-vbus-comparator go together");
/* Fast-loop interrupt: end of the injected sequence of the master ADC. */
#define ADC_IRQN DT_IRQN(ADCM_NODE)
#define ADC_IRQ_PRIO 0

PINCTRL_DT_DEFINE(PS_NODE);

#if HAS_HW_OC
BUILD_ASSERT(DT_PROP_LEN(PS_NODE, hw_trip_current_comparators) == 3,
	     "three current comparators expected (U, V, W)");
DT_FOREACH_PROP_ELEM(PS_NODE, hw_trip_current_comparators, STM32_PA_TRIP_COMP_OKAY)
static const struct stm32_pa_trip_comp oc_comps[] = {
	DT_FOREACH_PROP_ELEM_SEP(PS_NODE, hw_trip_current_comparators, STM32_PA_TRIP_COMP_BY_IDX, (,))};
#endif
#if HAS_HW_OV
STM32_PA_TRIP_COMP_OKAY(PS_NODE, hw_trip_vbus_comparator, 0)
static const struct stm32_pa_trip_comp ov_comp =
	STM32_PA_TRIP_COMP_BY_IDX(PS_NODE, hw_trip_vbus_comparator, 0);
#endif

#define OPAMP_DEV(idx, _) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(PS_NODE, opamps, idx))
static const struct device *const opamps[] = {LISTIFY(3, OPAMP_DEV, (,))};

static const struct stm32_pclken tim_clk[] = STM32_DT_CLOCKS(TIM_NODE);
static const struct stm32_pclken adc_clk[] = STM32_DT_CLOCKS(ADC_COMMON_NODE);

static struct power_stage_info info;

/* Shared with the zero-latency ISR: written before the interrupt is enabled
 * or as single aligned words.
 */
static volatile power_stage_fast_cb_t fast_fn;
static void *volatile fast_ctx;
static volatile uint16_t offset_u = ADC_MID;
static volatile uint16_t offset_v = ADC_MID;
static volatile uint16_t offset_w = ADC_MID;
static volatile bool offsets_valid;     /* the W trip means nothing before calibration */
static volatile uint16_t leg_trip_counts; /* 0 = disabled */
static volatile uint32_t isr_count;
static volatile uint16_t raw_temp; /* last NTC reading, 0 = none yet */
static volatile uint32_t fast_cycles_max;
static volatile bool hw_trip_latched; /* a break input fired: outputs off until reset */

/* Overrun of the fast loop: counted, reported to the callback once */
static volatile uint32_t overruns;
static volatile bool overrun_pending;

/* Offset calibration, accumulated by the ISR */
static volatile uint32_t cal_remaining;
static volatile uint32_t cal_sum_u;
static volatile uint32_t cal_sum_v;
static volatile uint32_t cal_sum_w;

/* ------------------------------------------------------------------------ */

static uint16_t recentre(uint16_t raw, uint16_t offset)
{
	const int32_t v = (int32_t)raw - (int32_t)offset + (int32_t)ADC_MID;

	if (v < 0) {
		return 0U;
	}
	return (v > (int32_t)ADC_FULL) ? (uint16_t)ADC_FULL : (uint16_t)v;
}

ISR_DIRECT_DECLARE(power_stage_adc_isr)
{
	if (LL_ADC_IsActiveFlag_JEOS(ADC_M) != 0U) {
		const uint16_t raw_u = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_1);
		const uint16_t raw_vbus = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_2);
		const uint16_t raw_w = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_3);
		const uint16_t raw_v = LL_ADC_INJ_ReadConversionData12(ADC_S, LL_ADC_INJ_RANK_1);
#if HAS_NTC
		raw_temp = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_4);
#endif
		const power_stage_fast_cb_t fn = fast_fn;

		LL_ADC_ClearFlag_JEOS(ADC_M);
		LL_ADC_ClearFlag_JEOS(ADC_S);
		isr_count++;

		if (cal_remaining > 0U) {
			cal_sum_u += raw_u;
			cal_sum_v += raw_v;
			cal_sum_w += raw_w;
			cal_remaining--;
		} else if (fn != NULL) {
			const uint32_t start = stm32_pa_cycles();
			const struct power_stage_sample sample = {
				.i_a = recentre(raw_u, offset_u),
				.i_b = recentre(raw_v, offset_v),
				.vbus = raw_vbus,
				.i_aux = recentre(raw_w, offset_w),
			};
			const int32_t trip = (int32_t)leg_trip_counts;
			uint32_t trips = 0U;

			/* Every leg, also in voltage drive where Stepper-C does not check */
			if ((trip != 0) && offsets_valid &&
			    ((abs((int32_t)raw_u - (int32_t)offset_u) > trip) ||
			     (abs((int32_t)raw_v - (int32_t)offset_v) > trip) ||
			     (abs((int32_t)raw_w - (int32_t)offset_w) > trip))) {
				trips |= POWER_STAGE_TRIP_AUX;
			}
			if (overrun_pending) {
				overrun_pending = false;
				trips |= POWER_STAGE_TRIP_OVERRUN;
			}
#if HAS_HW_OC
			if (LL_TIM_IsActiveFlag_BRK(TIMx) != 0U) {
				LL_TIM_ClearFlag_BRK(TIMx);
				hw_trip_latched = true;
				trips |= POWER_STAGE_TRIP_HW_OC;
			}
#endif
#if HAS_HW_OV
			if (LL_TIM_IsActiveFlag_BRK2(TIMx) != 0U) {
				LL_TIM_ClearFlag_BRK2(TIMx);
				hw_trip_latched = true;
				trips |= POWER_STAGE_TRIP_HW_OV;
			}
#endif

			fn(&sample, trips, fast_ctx);
			const uint32_t cycles = stm32_pa_cycles() - start;

			if (cycles > fast_cycles_max) {
				fast_cycles_max = cycles;
			}

			/* The next injected sequence ended while the callback ran: the
			 * period overran. Outputs off now, in this interrupt, whatever the
			 * callback does; the callback learns it with the next sample.
			 */
			if (LL_ADC_IsActiveFlag_JEOS(ADC_M) != 0U) {
				LL_TIM_DisableAllOutputs(TIMx);
				overruns++;
				overrun_pending = true;
			}
		}
	}

	return 0; /* zero-latency interrupt: no rescheduling */
}

/* ------------------------------------------------------------------------ */

static int opamps_init(void)
{
	const enum opamp_gain gain = DT_STRING_TOKEN(PS_NODE, opamp_gain);

	for (size_t i = 0; i < ARRAY_SIZE(opamps); i++) {
		int ret;

		if (!device_is_ready(opamps[i])) {
			LOG_ERR("%s not ready", opamps[i]->name);
			return -ENODEV;
		}
		ret = opamp_set_gain(opamps[i], gain);
		if (ret != 0) {
			LOG_ERR("%s: gain (%d)", opamps[i]->name, ret);
			return ret;
		}
	}
	return 0;
}

static void timer_init(void)
{
	TIM_TypeDef *const tim = TIMx;
	const uint32_t arr = info.timer_clock_hz / (2U * info.pwm_freq_hz);
	const uint32_t dt_ticks = stm32_pa_ns_to_ticks(info.dead_time_ns, info.timer_clock_hz);
	static const uint32_t legs[] = {LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH2,
					LL_TIM_CHANNEL_CH3};
	static const uint32_t legs_n[] = {LL_TIM_CHANNEL_CH1N, LL_TIM_CHANNEL_CH2N,
					  LL_TIM_CHANNEL_CH3N};

	info.pwm_period_ticks = (uint16_t)arr;
	/* Compare ticks = timer ticks: a leg at CCR = ARR - W is low for W ticks on each
	 * side of the top of the counter
	 */
	info.sample_window_ticks = (uint16_t)MIN(
		stm32_pa_ns_to_ticks(DT_PROP(PS_NODE, sample_window_ns), info.timer_clock_hz),
		arr / 4U);

	stm32_pa_timer_base(tim, arr);
	for (size_t i = 0; i < ARRAY_SIZE(legs); i++) {
		stm32_pa_timer_leg(tim, legs[i], legs_n[i], arr);
	}
	(void)stm32_pa_timer_adc_trigger(tim, LL_TIM_CHANNEL_CH4, arr);

	/* Off-state: with MOE = 0 the outputs go to their idle level (low), so
	 * both switches of every leg are off (legs high impedance).
	 */
	LL_TIM_SetOffStates(tim, LL_TIM_OSSI_ENABLE, LL_TIM_OSSR_ENABLE);
	LL_TIM_OC_SetDeadTime(tim, stm32_pa_dead_time_dtg(dt_ticks));

	LL_TIM_CC_EnableChannel(tim, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH1N |
					     LL_TIM_CHANNEL_CH2 | LL_TIM_CHANNEL_CH2N |
					     LL_TIM_CHANNEL_CH3 | LL_TIM_CHANNEL_CH3N);

	stm32_pa_timer_start(tim);

	LOG_INF("TIM1: %u Hz clock, ARR %u (%u Hz), dead time %u ns (%u ticks), sample window "
		"%u ticks",
		info.timer_clock_hz, arr, info.pwm_freq_hz, info.dead_time_ns, dt_ticks,
		info.sample_window_ticks);
}

#if HAS_HW_OC || HAS_HW_OV
/* The comparators run from their driver's init: only the DAC levels and the break
 * routing are left.
 */
static int hw_trips_init(void)
{
	uint32_t brk = 0U;
	uint32_t brk2 = 0U;
	int ret;

#if HAS_HW_OC
	for (size_t i = 0; i < ARRAY_SIZE(oc_comps); i++) {
		ret = stm32_pa_trip_comp_init(&oc_comps[i]);
		if (ret != 0) {
			return ret;
		}
		brk |= stm32_pa_break_source(oc_comps[i].regs);
	}
#endif
#if HAS_HW_OV
	ret = stm32_pa_trip_comp_init(&ov_comp);
	if (ret != 0) {
		return ret;
	}
	brk2 = stm32_pa_break_source(ov_comp.regs);
#endif
	ARG_UNUSED(ret);
	k_busy_wait(10); /* comparator start-up, DAC settling */

	stm32_pa_break_init(TIMx, brk, brk2);
	return 0;
}

#if HAS_HW_OC
/* Gain from the non-inverting input of the amplifiers to the ADC, x1000 */
static uint32_t oc_input_gain_milli(void)
{
#if DT_NODE_HAS_PROP(PS_NODE, hw_trip_current_input_gain_milli)
	return DT_PROP(PS_NODE, hw_trip_current_input_gain_milli);
#else
	/* PGA of the on-chip OPAMPs */
	switch (DT_STRING_TOKEN(PS_NODE, opamp_gain)) {
	case OPAMP_GAIN_2:
		return 2000U;
	case OPAMP_GAIN_4:
		return 4000U;
	case OPAMP_GAIN_8:
		return 8000U;
	case OPAMP_GAIN_16:
		return 16000U;
	case OPAMP_GAIN_32:
		return 32000U;
	case OPAMP_GAIN_64:
		return 64000U;
	default:
		return 1000U; /* level too high: the software trips still act */
	}
#endif
}
#endif

/* Levels from the calibrated offsets */
static void hw_trips_arm(void)
{
#if HAS_HW_OC
	const uint32_t trip_ma = DT_PROP(PS_NODE, hw_trip_current_milliamp);
	const uint16_t offsets[] = {offset_u, offset_v, offset_w};
	uint16_t level[ARRAY_SIZE(oc_comps)];

	BUILD_ASSERT(ARRAY_SIZE(offsets) == ARRAY_SIZE(oc_comps));
	for (size_t i = 0; i < ARRAY_SIZE(oc_comps); i++) {
		level[i] = hw_trip_oc_dac(offsets[i], trip_ma, info.i_full_scale_ma,
					  oc_input_gain_milli());
	}
	/* Comparators that share a DAC channel get the highest of their levels */
	for (size_t i = 0; i < ARRAY_SIZE(oc_comps); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(oc_comps); j++) {
			if ((oc_comps[j].dac == oc_comps[i].dac) &&
			    (oc_comps[j].dac_channel == oc_comps[i].dac_channel)) {
				level[i] = MAX(level[i], level[j]);
			}
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(oc_comps); i++) {
		(void)dac_write_value(oc_comps[i].dac, oc_comps[i].dac_channel, level[i]);
	}
	LOG_INF("hardware overcurrent trip %u mA: DAC levels U %u, V %u, W %u", trip_ma, level[0],
		level[1], level[2]);
#endif
#if HAS_HW_OV
	const uint32_t trip_mv = DT_PROP(PS_NODE, hw_trip_vbus_mv);
	const uint16_t dac_vbus = hw_trip_vbus_dac(trip_mv, info.adc_vbus_mv_full, ADC_FULL);

	(void)dac_write_value(ov_comp.dac, ov_comp.dac_channel, dac_vbus);
	LOG_INF("hardware overvoltage trip %u mV: DAC level %u", trip_mv, dac_vbus);
#endif
}

#endif /* HAS_HW_OC || HAS_HW_OV */

static int adcs_init(void)
{
	int ret;

	stm32_pa_adc_common_clock(ADC_M);

	ret = stm32_pa_adc_setup(ADC_M);
	if (ret == 0) {
		ret = stm32_pa_adc_setup(ADC_S);
	}
	if (ret != 0) {
		LOG_ERR("ADC setup failed (%d)", ret);
		return ret;
	}

	/* Amplifier outputs are low impedance; the divider (~16 kOhm) needs more */
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_U, LL_ADC_SAMPLINGTIME_6CYCLES_5);
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_VBUS, LL_ADC_SAMPLINGTIME_24CYCLES_5);
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_W, LL_ADC_SAMPLINGTIME_6CYCLES_5);
	LL_ADC_SetChannelSamplingTime(ADC_S, CH_V, LL_ADC_SAMPLINGTIME_6CYCLES_5);

#if HAS_NTC
	/* After the currents: about 0.9 us more before the fast loop starts */
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_TEMP, LL_ADC_SAMPLINGTIME_24CYCLES_5);
	LL_ADC_INJ_ConfigQueueContext(ADC_M, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_ENABLE_4RANKS,
				      CH_U, CH_VBUS, CH_W, CH_TEMP);
#else
	LL_ADC_INJ_ConfigQueueContext(ADC_M, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_ENABLE_3RANKS,
				      CH_U, CH_VBUS, CH_W, CH_U);
#endif
	LL_ADC_INJ_ConfigQueueContext(ADC_S, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_DISABLE, CH_V,
				      CH_V, CH_V, CH_V);

	LL_ADC_ClearFlag_JEOS(ADC_M);
	LL_ADC_ClearFlag_JEOS(ADC_S);
	LL_ADC_EnableIT_JEOS(ADC_M);

	LL_ADC_INJ_StartConversion(ADC_S);
	LL_ADC_INJ_StartConversion(ADC_M);
	return 0;
}

static void scaling_init(void)
{
	info.topology = POWER_STAGE_SHARED_LEG;
	info.legs = 3U;
	info.pwm_freq_hz = DT_PROP(PS_NODE, pwm_frequency);
	info.dead_time_ns = DT_PROP(PS_NODE, dead_time_ns);
	info.outputs_allowed = info.dead_time_ns != 0U;
	power_stage_scale(&info, ADC_BITS, DT_PROP(PS_NODE, adc_vref_mv),
			  DT_PROP(PS_NODE, shunt_micro_ohms),
			  DT_PROP(PS_NODE, current_sense_gain_milli),
			  DT_PROP_BY_IDX(PS_NODE, vbus_divider, 0),
			  DT_PROP_BY_IDX(PS_NODE, vbus_divider, 1),
			  DT_PROP(PS_NODE, current_sense_invert));
}

static int stm32_init(const struct device *dev)
{
	int ret;

	ARG_UNUSED(dev);

	scaling_init();

	ret = stm32_pa_clocks_on(tim_clk, adc_clk, &info.timer_clock_hz);
	if (ret != 0) {
		return ret;
	}

	ret = opamps_init();
	if (ret != 0) {
		return ret;
	}

	/* Timer first, with MOE = 0: when the pins switch to TIM1 they are
	 * already driven to the idle level (all switches off).
	 */
	timer_init();
#if HAS_HW_OC || HAS_HW_OV
	ret = hw_trips_init();
	if (ret != 0) {
		return ret;
	}
#endif

	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(PS_NODE), PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		LOG_ERR("pinctrl (%d)", ret);
		return ret;
	}

	ret = adcs_init();
	if (ret != 0) {
		return ret;
	}

	/* Cycle counter for the fast_cycles_max statistic */
	stm32_pa_cycle_counter_on();

	IRQ_DIRECT_CONNECT(ADC_IRQN, ADC_IRQ_PRIO, power_stage_adc_isr, IRQ_ZERO_LATENCY);
	irq_enable(ADC_IRQN);

	if (!info.outputs_allowed) {
		LOG_WRN("dead-time-ns is 0: set it in the devicetree (MCSDK value); "
			"the outputs stay disabled");
	}
	LOG_INF("current full scale %u mA (span %d), VBUS full scale %u mV",
		info.i_full_scale_ma, info.adc_i_span, info.adc_vbus_mv_full);
	return 0;
}

static int stm32_get_info(const struct device *dev, struct power_stage_info *out)
{
	ARG_UNUSED(dev);
	*out = info;
	/* Not before a successful calibration, never after a hardware trip */
	out->outputs_allowed = info.outputs_allowed && offsets_valid && !hw_trip_latched;
	return 0;
}

static int stm32_calibrate(const struct device *dev, uint32_t samples)
{
	const int64_t deadline = k_uptime_get() + 1000 + (int64_t)samples / 10;

	ARG_UNUSED(dev);

	if ((samples == 0U) || ((TIMx->BDTR & TIM_BDTR_MOE) != 0U)) {
		return -EINVAL;
	}

	offsets_valid = false;
	cal_sum_u = 0U;
	cal_sum_v = 0U;
	cal_sum_w = 0U;
	cal_remaining = samples; /* the ISR starts accumulating */

	while (cal_remaining > 0U) {
		if (k_uptime_get() > deadline) {
			cal_remaining = 0U;
			LOG_ERR("offset calibration timed out (no ADC interrupts?)");
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}

	offset_u = (uint16_t)((cal_sum_u + samples / 2U) / samples);
	offset_v = (uint16_t)((cal_sum_v + samples / 2U) / samples);
	offset_w = (uint16_t)((cal_sum_w + samples / 2U) / samples);
	LOG_INF("current offsets U %u, V %u, W %u counts", offset_u, offset_v, offset_w);
#if HAS_OFFSET_WINDOW
	{
		const uint16_t lo = DT_PROP(PS_NODE, current_offset_min_counts);
		const uint16_t hi = DT_PROP(PS_NODE, current_offset_max_counts);

		if (!hw_offset_in_window(offset_u, lo, hi) || !hw_offset_in_window(offset_v, lo, hi) ||
		    !hw_offset_in_window(offset_w, lo, hi)) {
			LOG_ERR("current offsets outside %u..%u: amplifier, shunt or bias fault; "
				"outputs refused", lo, hi);
			return -EIO;
		}
	}
#endif
#if HAS_HW_OC || HAS_HW_OV
	hw_trips_arm();
#endif
	offsets_valid = true;
	return 0;
}

static int stm32_set_fast_callback(const struct device *dev, power_stage_fast_cb_t cb, void *ctx)
{
	ARG_UNUSED(dev);
	fast_fn = NULL;
	fast_ctx = ctx;
	fast_fn = cb;
	return 0;
}

static int stm32_set_aux_trip(const struct device *dev, uint32_t trip_ma)
{
	uint32_t counts = 0U;

	ARG_UNUSED(dev);

	if ((trip_ma != 0U) && (info.i_full_scale_ma != 0U)) {
		counts = (uint32_t)((uint64_t)trip_ma * ADC_MID / info.i_full_scale_ma);
		counts = MAX(counts, 1U);
	}
	leg_trip_counts = (uint16_t)MIN(counts, ADC_MID);
	return 0;
}

static int stm32_write(const struct device *dev, const uint16_t *compare, size_t legs)
{
	TIM_TypeDef *const tim = TIMx;

	ARG_UNUSED(dev);

	if (legs < 3U) {
		return -EINVAL;
	}
	LL_TIM_OC_SetCompareCH1(tim, compare[0]);
	LL_TIM_OC_SetCompareCH2(tim, compare[1]);
	LL_TIM_OC_SetCompareCH3(tim, compare[2]);
	return 0;
}

static int stm32_output_enable(const struct device *dev, bool enable)
{
	TIM_TypeDef *const tim = TIMx;

	ARG_UNUSED(dev);

	if (enable) {
		if (!info.outputs_allowed || !offsets_valid) {
			return -EPERM;
		}
		if (hw_trip_latched) {
			return -EIO;
		}
		LL_TIM_EnableAllOutputs(tim);
	} else {
		LL_TIM_DisableAllOutputs(tim);
	}
	return 0;
}

static int stm32_get_stats(const struct device *dev, struct power_stage_stats *stats)
{
	ARG_UNUSED(dev);
	stats->isr_count = isr_count;
	stats->fast_cycles_max = fast_cycles_max;
	stats->overruns = overruns;
	fast_cycles_max = 0U;
	return 0;
}

static int stm32_get_temperature(const struct device *dev, int16_t *decidegc)
{
	ARG_UNUSED(dev);
#if HAS_NTC
	const uint16_t counts = raw_temp;

	if (counts == 0U) {
		return -EAGAIN; /* no conversion yet */
	}
	return ntc_decidegc(counts, ADC_FULL, DT_PROP(PS_NODE, ntc_r25_ohm),
			    DT_PROP(PS_NODE, ntc_beta), DT_PROP(PS_NODE, ntc_bottom_ohm), decidegc);
#else
	ARG_UNUSED(decidegc);
	return -ENOSYS;
#endif
}

static DEVICE_API(power_stage, stm32_api) = {
	.get_info = stm32_get_info,
	.calibrate = stm32_calibrate,
	.set_fast_callback = stm32_set_fast_callback,
	.write = stm32_write,
	.output_enable = stm32_output_enable,
	.set_aux_trip = stm32_set_aux_trip,
	.get_stats = stm32_get_stats,
	.get_temperature = stm32_get_temperature,
};

DEVICE_DT_INST_DEFINE(0, stm32_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_POWER_STAGE_INIT_PRIORITY, &stm32_api);
