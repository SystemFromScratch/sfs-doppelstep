/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Building blocks of the STM32 power stage backends: the advanced-control timer in
 * center-aligned PWM with an ADC trigger at the top of the counter, the injected
 * conversions of the ADCs, the break inputs fed by comparators, the cycle counter.
 *
 * Why LL here and not Zephyr's PWM and ADC drivers: their APIs have no timer-triggered
 * injected conversions, no update of all compare values in one period, no trigger
 * output and no break inputs, and they may not be called from the zero-latency
 * interrupt of the fast loop (docs/architecture.md). Comparators, DACs and OPAMPs,
 * which only need a configuration, belong to their Zephyr drivers.
 *
 * Only the backends power_stage_stm32*.c include this header (scripts/check-layers.sh).
 */

#ifndef DOPPELSTEP_POWER_STAGE_STM32_PWM_ADC_H_
#define DOPPELSTEP_POWER_STAGE_STM32_PWM_ADC_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/dac.h>

#include <soc.h>
#include <stm32_ll_adc.h>
#include <stm32_ll_dac.h>
#include <stm32_ll_tim.h>

/** Timer ticks of @p ns at @p clock_hz, rounded up. */
uint32_t stm32_pa_ns_to_ticks(uint32_t ns, uint32_t clock_hz);

/** TIMx_BDTR.DTG for a dead time of @p ticks timer clock periods (CKD = 1). */
uint32_t stm32_pa_dead_time_dtg(uint32_t ticks);

/**
 * Bus clocks of the timer and of the ADC common block on; the kernel clock of the
 * timer (second clock cell of its node) into @p timer_clock_hz.
 */
int stm32_pa_clocks_on(const struct stm32_pclken *tim_clk, const struct stm32_pclken *adc_clk,
		       uint32_t *timer_clock_hz);

/**
 * Stop the timer with MOE = 0 and set up the counter: center-aligned (0 -> ARR -> 0),
 * no prescaler, ARR preloaded. The outputs stay at their idle level.
 */
void stm32_pa_timer_base(TIM_TypeDef *tim, uint32_t arr);

/**
 * One PWM leg: PWM mode 1 (output high while CNT < CCR), active high, idle low,
 * compare preloaded, at 50 %. @p channel_n is the complementary channel, or 0 for
 * a stage whose gate driver makes the low-side signal itself.
 */
void stm32_pa_timer_leg(TIM_TypeDef *tim, uint32_t channel, uint32_t channel_n, uint32_t arr);

/**
 * ADC trigger on TRGO2: @p channel (LL_TIM_CHANNEL_CH4, _CH5 or _CH6) in PWM mode 2 at
 * ARR - 1, so its reference rises just before the top of the counter, where every leg
 * is on its low-side switch. CH5 and CH6 have no pin.
 * @retval -EINVAL another channel.
 */
int stm32_pa_timer_adc_trigger(TIM_TypeDef *tim, uint32_t channel, uint32_t arr);

/** Load the preloaded registers and start the counter (outputs still off: MOE = 0). */
void stm32_pa_timer_start(TIM_TypeDef *tim);

/** Synchronous ADC clock HCLK / 4 (42.5 MHz at 170 MHz) for the ADCs of @p adc's common block. */
void stm32_pa_adc_common_clock(ADC_TypeDef *adc);

/** Wake up, calibrate (single ended), 12 bit right aligned, injected queue off, enable. */
int stm32_pa_adc_setup(ADC_TypeDef *adc);

/**
 * Source bit of the break inputs for the comparator at @p comp
 * (LL_TIM_BKIN_SOURCE_BKCOMPx), 0 if the comparator cannot drive a break input.
 */
uint32_t stm32_pa_break_source(const COMP_TypeDef *comp);

/**
 * Break input BRK from the comparators of @p brk_sources and BRK2 from those of
 * @p brk2_sources (ORed stm32_pa_break_source() values; 0 = input off), filtered for
 * about 0.5 us at 170 MHz, active high, flags cleared. The break pins are not used.
 */
void stm32_pa_break_init(TIM_TypeDef *tim, uint32_t brk_sources, uint32_t brk2_sources);

/**
 * High-frequency interface mode of a DAC (DAC_MCR.HFSEL) for the AHB clock: required
 * above 80 MHz (RM0440, DAC). The Zephyr DAC driver of v4.5 does not set it, so the
 * backend calls this before dac_channel_setup().
 */
void stm32_pa_dac_hfsel(DAC_TypeDef *dac, uint32_t hclk_hz);

/**
 * @name Hardware trip comparators
 *
 * A comparator node (st,stm32g4-comp) of the Zephyr comparator driver whose negative
 * input names a DAC channel ("DAC3_CH1"): the DAC channel holds the trip level, the
 * comparator output drives a break input of the timer (stm32_pa_break_init()).
 * @{
 */
struct stm32_pa_trip_comp {
	const struct device *comp;
	const struct device *dac;
	COMP_TypeDef *regs;
	DAC_TypeDef *dac_regs;
	uint8_t dac_channel;
};

#define STM32_PA_DAC_NODE_DAC1_CH1 DT_NODELABEL(dac1)
#define STM32_PA_DAC_NODE_DAC1_CH2 DT_NODELABEL(dac1)
#define STM32_PA_DAC_NODE_DAC2_CH1 DT_NODELABEL(dac2)
#define STM32_PA_DAC_NODE_DAC3_CH1 DT_NODELABEL(dac3)
#define STM32_PA_DAC_NODE_DAC3_CH2 DT_NODELABEL(dac3)
#define STM32_PA_DAC_NODE_DAC4_CH1 DT_NODELABEL(dac4)
#define STM32_PA_DAC_NODE_DAC4_CH2 DT_NODELABEL(dac4)
#define STM32_PA_DAC_CH_DAC1_CH1   1U
#define STM32_PA_DAC_CH_DAC1_CH2   2U
#define STM32_PA_DAC_CH_DAC2_CH1   1U
#define STM32_PA_DAC_CH_DAC3_CH1   1U
#define STM32_PA_DAC_CH_DAC3_CH2   2U
#define STM32_PA_DAC_CH_DAC4_CH1   1U
#define STM32_PA_DAC_CH_DAC4_CH2   2U

/* A comparator whose negative input is no DAC channel fails to compile here */
#define STM32_PA_DAC_NODE(cnode) CONCAT(STM32_PA_DAC_NODE_, DT_STRING_TOKEN(cnode, negative_input))
#define STM32_PA_DAC_CH(cnode)   CONCAT(STM32_PA_DAC_CH_, DT_STRING_TOKEN(cnode, negative_input))

/** Initialiser of a struct stm32_pa_trip_comp from a comparator node. */
#define STM32_PA_TRIP_COMP(cnode)                                                                  \
	{                                                                                          \
		.comp = DEVICE_DT_GET(cnode),                                                      \
		.dac = DEVICE_DT_GET(STM32_PA_DAC_NODE(cnode)),                                    \
		.regs = (COMP_TypeDef *)DT_REG_ADDR(cnode),                                        \
		.dac_regs = (DAC_TypeDef *)DT_REG_ADDR(STM32_PA_DAC_NODE(cnode)),                  \
		.dac_channel = STM32_PA_DAC_CH(cnode),                                             \
	}

/** For DT_FOREACH_PROP_ELEM_SEP over a phandle(s) property of comparators. */
#define STM32_PA_TRIP_COMP_BY_IDX(node, prop, idx)                                                 \
	STM32_PA_TRIP_COMP(DT_PHANDLE_BY_IDX(node, prop, idx))

/** Build check: the comparator and its DAC are "okay". */
#define STM32_PA_TRIP_COMP_OKAY(node, prop, idx)                                                   \
	BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_PHANDLE_BY_IDX(node, prop, idx), okay) &&               \
			     DT_NODE_HAS_STATUS(                                                   \
				     STM32_PA_DAC_NODE(DT_PHANDLE_BY_IDX(node, prop, idx)), okay), \
		     "hardware trip comparators and their DACs must be okay");

/**
 * Check that the comparator and its DAC are ready, and set the DAC channel up for the
 * comparator only (internal, no pin) at full scale: no trip before the level is set.
 */
int stm32_pa_trip_comp_init(const struct stm32_pa_trip_comp *c);
/** @} */

/** Start the cycle counter (DWT) for the statistics of the fast loop. */
void stm32_pa_cycle_counter_on(void);

/** CPU cycles since the cycle counter started, for differences. */
static inline uint32_t stm32_pa_cycles(void)
{
	return DWT->CYCCNT;
}

#endif /* DOPPELSTEP_POWER_STAGE_STM32_PWM_ADC_H_ */
