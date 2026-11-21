/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Building blocks of the STM32 power stage backends (stm32_pwm_adc.h).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stm32_ll_bus.h>

#include "stm32_pwm_adc.h"

LOG_MODULE_DECLARE(power_stage, CONFIG_POWER_STAGE_LOG_LEVEL);

/* fDTS / 16, N = 5 at 170 MHz: 0.47 us, longer than the switching spikes */
#define BREAK_FILTER  LL_TIM_BREAK_FILTER_FDIV16_N5
#define BREAK2_FILTER LL_TIM_BREAK2_FILTER_FDIV16_N5

uint32_t stm32_pa_ns_to_ticks(uint32_t ns, uint32_t clock_hz)
{
	return (uint32_t)(((uint64_t)ns * clock_hz + 999999999U) / 1000000000U);
}

uint32_t stm32_pa_dead_time_dtg(uint32_t ticks)
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

int stm32_pa_clocks_on(const struct stm32_pclken *tim_clk, const struct stm32_pclken *adc_clk,
		       uint32_t *timer_clock_hz)
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
	*timer_clock_hz = rate;
	return 0;
}

void stm32_pa_timer_base(TIM_TypeDef *tim, uint32_t arr)
{
	LL_TIM_DisableCounter(tim);
	LL_TIM_DisableAllOutputs(tim);

	LL_TIM_SetPrescaler(tim, 0U);
	LL_TIM_SetCounterMode(tim, LL_TIM_COUNTERMODE_CENTER_UP);
	LL_TIM_SetClockDivision(tim, LL_TIM_CLOCKDIVISION_DIV1);
	LL_TIM_SetAutoReload(tim, arr);
	LL_TIM_EnableARRPreload(tim);
	LL_TIM_SetRepetitionCounter(tim, 0U);
}

static void set_compare(TIM_TypeDef *tim, uint32_t channel, uint32_t value)
{
	switch (channel) {
	case LL_TIM_CHANNEL_CH1:
		LL_TIM_OC_SetCompareCH1(tim, value);
		break;
	case LL_TIM_CHANNEL_CH2:
		LL_TIM_OC_SetCompareCH2(tim, value);
		break;
	case LL_TIM_CHANNEL_CH3:
		LL_TIM_OC_SetCompareCH3(tim, value);
		break;
	case LL_TIM_CHANNEL_CH4:
		LL_TIM_OC_SetCompareCH4(tim, value);
		break;
	case LL_TIM_CHANNEL_CH5:
		LL_TIM_OC_SetCompareCH5(tim, value);
		break;
	case LL_TIM_CHANNEL_CH6:
		LL_TIM_OC_SetCompareCH6(tim, value);
		break;
	default:
		break;
	}
}

void stm32_pa_timer_leg(TIM_TypeDef *tim, uint32_t channel, uint32_t channel_n, uint32_t arr)
{
	LL_TIM_OC_SetMode(tim, channel, LL_TIM_OCMODE_PWM1);
	LL_TIM_OC_ConfigOutput(tim, channel, LL_TIM_OCPOLARITY_HIGH | LL_TIM_OCIDLESTATE_LOW);
	if (channel_n != 0U) {
		LL_TIM_OC_ConfigOutput(tim, channel_n,
				       LL_TIM_OCPOLARITY_HIGH | LL_TIM_OCIDLESTATE_LOW);
	}
	LL_TIM_OC_EnablePreload(tim, channel);
	set_compare(tim, channel, arr / 2U);
}

int stm32_pa_timer_adc_trigger(TIM_TypeDef *tim, uint32_t channel, uint32_t arr)
{
	uint32_t trgo2;

	switch (channel) {
	case LL_TIM_CHANNEL_CH4:
		trgo2 = LL_TIM_TRGO2_OC4;
		break;
	case LL_TIM_CHANNEL_CH5:
		trgo2 = LL_TIM_TRGO2_OC5;
		break;
	case LL_TIM_CHANNEL_CH6:
		trgo2 = LL_TIM_TRGO2_OC6;
		break;
	default:
		return -EINVAL;
	}

	/* OCxREF rising just before the top of the counter */
	LL_TIM_OC_SetMode(tim, channel, LL_TIM_OCMODE_PWM2);
	set_compare(tim, channel, arr - 1U);
	LL_TIM_OC_EnablePreload(tim, channel);
	LL_TIM_SetTriggerOutput2(tim, trgo2);
	return 0;
}

void stm32_pa_timer_start(TIM_TypeDef *tim)
{
	LL_TIM_GenerateEvent_UPDATE(tim);
	LL_TIM_EnableCounter(tim);
}

void stm32_pa_adc_common_clock(ADC_TypeDef *adc)
{
	/* Synchronous ADC clock: HCLK / 4 = 42.5 MHz (max. 60 MHz) */
	LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(adc), LL_ADC_CLOCK_SYNC_PCLK_DIV4);
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

int stm32_pa_adc_setup(ADC_TypeDef *adc)
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

uint32_t stm32_pa_break_source(const COMP_TypeDef *comp)
{
	if (comp == COMP1) {
		return LL_TIM_BKIN_SOURCE_BKCOMP1;
	}
	if (comp == COMP2) {
		return LL_TIM_BKIN_SOURCE_BKCOMP2;
	}
	if (comp == COMP3) {
		return LL_TIM_BKIN_SOURCE_BKCOMP3;
	}
	if (comp == COMP4) {
		return LL_TIM_BKIN_SOURCE_BKCOMP4;
	}
#if defined(COMP5)
	if (comp == COMP5) {
		return LL_TIM_BKIN_SOURCE_BKCOMP5;
	}
#endif
#if defined(COMP6)
	if (comp == COMP6) {
		return LL_TIM_BKIN_SOURCE_BKCOMP6;
	}
#endif
#if defined(COMP7)
	if (comp == COMP7) {
		return LL_TIM_BKIN_SOURCE_BKCOMP7;
	}
#endif
	return 0U;
}

void stm32_pa_break_init(TIM_TypeDef *tim, uint32_t brk_sources, uint32_t brk2_sources)
{
	/* The break pins are not used: their sources are enabled after reset */
	LL_TIM_DisableBreakInputSource(tim, LL_TIM_BREAK_INPUT_BKIN, LL_TIM_BKIN_SOURCE_BKIN);
	LL_TIM_DisableBreakInputSource(tim, LL_TIM_BREAK_INPUT_BKIN2, LL_TIM_BKIN_SOURCE_BKIN);

	if (brk_sources != 0U) {
		LL_TIM_EnableBreakInputSource(tim, LL_TIM_BREAK_INPUT_BKIN, brk_sources);
		LL_TIM_ConfigBRK(tim, LL_TIM_BREAK_POLARITY_HIGH, BREAK_FILTER,
				 LL_TIM_BREAK_AFMODE_INPUT);
		LL_TIM_ClearFlag_BRK(tim);
		LL_TIM_EnableBRK(tim);
	}
	if (brk2_sources != 0U) {
		LL_TIM_EnableBreakInputSource(tim, LL_TIM_BREAK_INPUT_BKIN2, brk2_sources);
		LL_TIM_ConfigBRK2(tim, LL_TIM_BREAK2_POLARITY_HIGH, BREAK2_FILTER,
				  LL_TIM_BREAK2_AFMODE_INPUT);
		LL_TIM_ClearFlag_BRK2(tim);
		LL_TIM_EnableBRK2(tim);
	}
}

void stm32_pa_dac_hfsel(DAC_TypeDef *dac, uint32_t hclk_hz)
{
	uint32_t mode = LL_DAC_HIGH_FREQ_MODE_DISABLE;

	if (hclk_hz > 160000000U) {
		mode = LL_DAC_HIGH_FREQ_MODE_ABOVE_160MHZ;
	} else if (hclk_hz > 80000000U) {
		mode = LL_DAC_HIGH_FREQ_MODE_ABOVE_80MHZ;
	}
	LL_DAC_SetHighFrequencyMode(dac, mode);
}

#if defined(CONFIG_DAC)
#include <zephyr/drivers/dac.h>

int stm32_pa_trip_comp_init(const struct stm32_pa_trip_comp *c)
{
	const struct dac_channel_cfg cfg = {
		.channel_id = c->dac_channel,
		.resolution = 12U,
		.buffered = false,
		.internal = true,
	};
	int ret;

	if (!device_is_ready(c->comp) || !device_is_ready(c->dac)) {
		LOG_ERR("%s or %s not ready", c->comp->name, c->dac->name);
		return -ENODEV;
	}
	stm32_pa_dac_hfsel(c->dac_regs, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	ret = dac_channel_setup(c->dac, &cfg);
	if (ret == 0) {
		ret = dac_write_value(c->dac, c->dac_channel, (1U << 12) - 1U);
	}
	if (ret != 0) {
		LOG_ERR("%s channel %u (%d)", c->dac->name, c->dac_channel, ret);
	}
	return ret;
}
#endif /* CONFIG_DAC */

void stm32_pa_cycle_counter_on(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
