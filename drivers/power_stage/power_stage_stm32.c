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
 */

#define DT_DRV_COMPAT sfs_stm32_shared_leg_power_stage

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/opamp.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stm32_ll_adc.h>
#include <stm32_ll_tim.h>

#include <doppelstep/drivers/power_stage.h>

#include "power_stage_scaling.h"

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

/* Fast-loop interrupt: end of the injected sequence of the master ADC. */
#define ADC_IRQN DT_IRQN(ADCM_NODE)
#define ADC_IRQ_PRIO 0

PINCTRL_DT_DEFINE(PS_NODE);

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
static volatile uint16_t w_trip_counts; /* 0 = disabled */
static volatile uint32_t isr_count;
static volatile uint32_t fast_cycles_max;

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
			const uint32_t start = DWT->CYCCNT;
			const struct power_stage_sample sample = {
				.i_a = recentre(raw_u, offset_u),
				.i_b = recentre(raw_v, offset_v),
				.vbus = raw_vbus,
			};
			const uint16_t trip = w_trip_counts;
			const bool w_trip =
				(trip != 0U) &&
				((uint16_t)abs((int32_t)raw_w - (int32_t)offset_w) > trip);

			fn(&sample, w_trip, fast_ctx);
			const uint32_t cycles = DWT->CYCCNT - start;

			if (cycles > fast_cycles_max) {
				fast_cycles_max = cycles;
			}
		}
	}

	return 0; /* zero-latency interrupt: no rescheduling */
}

/* ------------------------------------------------------------------------ */

/* TIMx_BDTR.DTG for a dead time of `ticks` timer clock periods (CKD = 1). */
static uint32_t dead_time_dtg(uint32_t ticks)
{
	if (ticks <= 127U) {
		return ticks;
	}
	if (ticks <= 2U * (64U + 63U)) {
		return 0x80U | ((ticks + 1U) / 2U - 64U);
	}
	if (ticks <= 8U * (32U + 31U)) {
		return 0xC0U | ((ticks + 7U) / 8U - 32U);
	}
	if (ticks <= 16U * (32U + 31U)) {
		return 0xE0U | ((ticks + 15U) / 16U - 32U);
	}
	return 0xFFU;
}

static int clocks_on(void)
{
	const struct device *const clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	uint32_t rate = 0U;
	int ret;

	if (!device_is_ready(clk)) {
		return -ENODEV;
	}

	ret = clock_control_on(clk, (clock_control_subsys_t)&tim_clk[0]);
	if (ret == 0) {
		ret = clock_control_on(clk, (clock_control_subsys_t)&adc_clk[0]);
	}
	if (ret == 0) {
		/* Second clock cell of the timer: kernel clock (TIMPCLKx) */
		ret = clock_control_get_rate(clk, (clock_control_subsys_t)&tim_clk[1], &rate);
	}
	if (ret != 0) {
		LOG_ERR("clock setup failed (%d)", ret);
		return ret;
	}
	info.timer_clock_hz = rate;
	return 0;
}

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
	const uint32_t dt_ticks =
		(uint32_t)(((uint64_t)info.dead_time_ns * info.timer_clock_hz + 999999999U) /
			   1000000000U);
	static const uint32_t legs[] = {LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH2,
					LL_TIM_CHANNEL_CH3};
	static const uint32_t legs_n[] = {LL_TIM_CHANNEL_CH1N, LL_TIM_CHANNEL_CH2N,
					  LL_TIM_CHANNEL_CH3N};

	info.pwm_period_ticks = (uint16_t)arr;

	LL_TIM_DisableCounter(tim);
	LL_TIM_DisableAllOutputs(tim);

	LL_TIM_SetPrescaler(tim, 0U);
	LL_TIM_SetCounterMode(tim, LL_TIM_COUNTERMODE_CENTER_UP);
	LL_TIM_SetClockDivision(tim, LL_TIM_CLOCKDIVISION_DIV1);
	LL_TIM_SetAutoReload(tim, arr);
	LL_TIM_EnableARRPreload(tim);
	LL_TIM_SetRepetitionCounter(tim, 0U);

	for (size_t i = 0; i < ARRAY_SIZE(legs); i++) {
		LL_TIM_OC_SetMode(tim, legs[i], LL_TIM_OCMODE_PWM1);
		LL_TIM_OC_ConfigOutput(tim, legs[i], LL_TIM_OCPOLARITY_HIGH | LL_TIM_OCIDLESTATE_LOW);
		LL_TIM_OC_ConfigOutput(tim, legs_n[i],
				       LL_TIM_OCPOLARITY_HIGH | LL_TIM_OCIDLESTATE_LOW);
		LL_TIM_OC_EnablePreload(tim, legs[i]);
	}
	LL_TIM_OC_SetCompareCH1(tim, arr / 2U);
	LL_TIM_OC_SetCompareCH2(tim, arr / 2U);
	LL_TIM_OC_SetCompareCH3(tim, arr / 2U);

	/* ADC trigger: OC4REF rising just before the top of the counter */
	LL_TIM_OC_SetMode(tim, LL_TIM_CHANNEL_CH4, LL_TIM_OCMODE_PWM2);
	LL_TIM_OC_SetCompareCH4(tim, arr - 1U);
	LL_TIM_OC_EnablePreload(tim, LL_TIM_CHANNEL_CH4);
	LL_TIM_SetTriggerOutput2(tim, LL_TIM_TRGO2_OC4);

	/* Off-state: with MOE = 0 the outputs go to their idle level (low), so
	 * both switches of every leg are off (legs high impedance).
	 */
	LL_TIM_SetOffStates(tim, LL_TIM_OSSI_ENABLE, LL_TIM_OSSR_ENABLE);
	LL_TIM_OC_SetDeadTime(tim, dead_time_dtg(dt_ticks));

	LL_TIM_CC_EnableChannel(tim, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH1N |
					     LL_TIM_CHANNEL_CH2 | LL_TIM_CHANNEL_CH2N |
					     LL_TIM_CHANNEL_CH3 | LL_TIM_CHANNEL_CH3N);

	LL_TIM_GenerateEvent_UPDATE(tim);
	LL_TIM_EnableCounter(tim);

	LOG_INF("TIM1: %u Hz clock, ARR %u (%u Hz), dead time %u ns (%u ticks)",
		info.timer_clock_hz, arr, info.pwm_freq_hz, info.dead_time_ns, dt_ticks);
}

static int adc_wait(ADC_TypeDef *adc, uint32_t (*busy)(const ADC_TypeDef *))
{
	for (uint32_t i = 0U; i < 100000U; i++) {
		if (busy(adc) == 0U) {
			return 0;
		}
		k_busy_wait(1);
	}
	return -ETIMEDOUT;
}

static uint32_t adc_cal_busy(const ADC_TypeDef *adc)
{
	return LL_ADC_IsCalibrationOnGoing(adc);
}

static uint32_t adc_not_ready(const ADC_TypeDef *adc)
{
	return (LL_ADC_IsActiveFlag_ADRDY(adc) != 0U) ? 0U : 1U;
}

static int adc_setup(ADC_TypeDef *adc)
{
	int ret;

	LL_ADC_DisableDeepPowerDown(adc);
	LL_ADC_EnableInternalRegulator(adc);
	k_busy_wait(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);

	LL_ADC_StartCalibration(adc, LL_ADC_SINGLE_ENDED);
	ret = adc_wait(adc, adc_cal_busy);
	if (ret != 0) {
		return ret;
	}
	/* LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES ADC clock cycles (>= 4 at 42.5 MHz) */
	k_busy_wait(1);

	LL_ADC_SetResolution(adc, LL_ADC_RESOLUTION_12B);
	LL_ADC_SetDataAlignment(adc, LL_ADC_DATA_ALIGN_RIGHT);
	LL_ADC_INJ_SetQueueMode(adc, LL_ADC_INJ_QUEUE_DISABLE);
	LL_ADC_INJ_SetTrigAuto(adc, LL_ADC_INJ_TRIG_INDEPENDENT);

	LL_ADC_ClearFlag_ADRDY(adc);
	LL_ADC_Enable(adc);
	return adc_wait(adc, adc_not_ready);
}

static int adcs_init(void)
{
	int ret;

	/* Synchronous ADC clock: HCLK / 4 = 42.5 MHz (max. 60 MHz) */
	LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC_M), LL_ADC_CLOCK_SYNC_PCLK_DIV4);

	ret = adc_setup(ADC_M);
	if (ret == 0) {
		ret = adc_setup(ADC_S);
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

	LL_ADC_INJ_ConfigQueueContext(ADC_M, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_ENABLE_3RANKS,
				      CH_U, CH_VBUS, CH_W, CH_U);
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

	ret = clocks_on();
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
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

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
	return 0;
}

static int stm32_calibrate(const struct device *dev, uint32_t samples)
{
	const int64_t deadline = k_uptime_get() + 1000 + (int64_t)samples / 10;

	ARG_UNUSED(dev);

	if ((samples == 0U) || ((TIMx->BDTR & TIM_BDTR_MOE) != 0U)) {
		return -EINVAL;
	}

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
	w_trip_counts = (uint16_t)MIN(counts, ADC_MID);
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
		if (!info.outputs_allowed) {
			return -EPERM;
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
	fast_cycles_max = 0U;
	return 0;
}

static DEVICE_API(power_stage, stm32_api) = {
	.get_info = stm32_get_info,
	.calibrate = stm32_calibrate,
	.set_fast_callback = stm32_set_fast_callback,
	.write = stm32_write,
	.output_enable = stm32_output_enable,
	.set_aux_trip = stm32_set_aux_trip,
	.get_stats = stm32_get_stats,
};

DEVICE_DT_INST_DEFINE(0, stm32_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_POWER_STAGE_INIT_PRIORITY, &stm32_api);
