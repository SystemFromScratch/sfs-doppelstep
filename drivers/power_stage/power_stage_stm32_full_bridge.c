/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32 backend of the power_stage class for two full H-bridges in one gate driver with
 * an integrated dead time and one PWM input per half-bridge (TI DRV8412 in mode
 * M3..M1 = 000): four PWM channels of an advanced-control timer without complementary
 * outputs, one low-side shunt per bridge, two ADCs with injected conversions triggered
 * by the timer, the reset inputs of the gate driver as the output switch. Hardware
 * description: the node with compatible "sfs,stm32-full-bridge-power-stage".
 *
 * Timing (center-aligned PWM, counter 0 -> ARR -> 0, one period = 2 * ARR ticks):
 *
 *   CNT  0 ........ ARR-1  ARR  ARR-1 ........ 0
 *   OCxREF          _______________                (x = adc-trigger-channel, PWM mode 2)
 *   TRGO2 ---------^  injected conversions of ADC1 (A, VBUS) and ADC2 (B, NTC)
 *
 * At the top of the counter all four PWM inputs are low (PWM mode 1: high while
 * CNT < CCRx), so every leg is on its low-side switch and the shunts in the low-side
 * paths of leg A+ and leg B- carry the coil currents. The end of the ADC1 injected
 * sequence runs the fast loop.
 *
 * Outputs off = both reset inputs asserted: the gate driver opens every switch. MOE of
 * the timer is switched together with them; with MOE = 0 the PWM inputs sit at their
 * idle level, low, which would close the low sides if the resets were released. The
 * optional hardware overvoltage trip (comparator into BRK2) clears MOE in hardware; the
 * interrupt then asserts the resets within one period.
 */

#define DT_DRV_COMPAT sfs_stm32_full_bridge_power_stage

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/gpio.h>
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

#define TIM_NODE        DT_PHANDLE(PS_NODE, timer)
#define ADCM_NODE       DT_PHANDLE(PS_NODE, adc_master)
#define ADCS_NODE       DT_PHANDLE(PS_NODE, adc_slave)
#define ADC_COMMON_NODE DT_PARENT(ADCM_NODE)

/* LL programs these peripherals directly: no Zephyr driver may own them. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(TIM_NODE, okay), "power stage timer must be disabled");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(ADCM_NODE, okay), "power stage adc-master must be disabled");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(ADCS_NODE, okay), "power stage adc-slave must be disabled");
BUILD_ASSERT(DT_SAME_NODE(ADC_COMMON_NODE, DT_PARENT(ADCS_NODE)),
	     "adc-master and adc-slave must share one ADC common block");
BUILD_ASSERT(DT_PROP_LEN(PS_NODE, vbus_divider) == 2, "vbus-divider = <R_low R_high>");
BUILD_ASSERT(DT_PROP_LEN(PS_NODE, reset_gpios) == 2, "reset-gpios: one per bridge");

#define ADC_BITS 12U
#define ADC_FULL ((1U << ADC_BITS) - 1U)
#define ADC_MID  (1U << (ADC_BITS - 1U))

#define TIMx  ((TIM_TypeDef *)DT_REG_ADDR(TIM_NODE))
#define ADC_M ((ADC_TypeDef *)DT_REG_ADDR(ADCM_NODE))
#define ADC_S ((ADC_TypeDef *)DT_REG_ADDR(ADCS_NODE))

#define CH_A    __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, phase_a_channel))
#define CH_B    __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, phase_b_channel))
#define CH_VBUS __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, vbus_channel))

#define TRIGGER_CHANNEL                                                                            \
	COND_CODE_1(IS_EQ(DT_PROP(PS_NODE, adc_trigger_channel), 4), (LL_TIM_CHANNEL_CH4),         \
		    (COND_CODE_1(IS_EQ(DT_PROP(PS_NODE, adc_trigger_channel), 5),                  \
				 (LL_TIM_CHANNEL_CH5), (LL_TIM_CHANNEL_CH6))))
BUILD_ASSERT(DT_PROP(PS_NODE, adc_trigger_channel) != 4,
	     "CH4 drives leg B-: trigger the ADCs from CH5 or CH6");

/* Optional NTC (temperature-channel): second injected rank of the slave ADC */
#define HAS_NTC DT_NODE_HAS_PROP(PS_NODE, temperature_channel)
#if HAS_NTC
BUILD_ASSERT(DT_NODE_HAS_PROP(PS_NODE, ntc_r25_ohm) && DT_NODE_HAS_PROP(PS_NODE, ntc_beta) &&
		     DT_NODE_HAS_PROP(PS_NODE, ntc_bottom_ohm),
	     "temperature-channel needs ntc-r25-ohm, ntc-beta and ntc-bottom-ohm");
#define CH_TEMP __LL_ADC_DECIMAL_NB_TO_CHANNEL(DT_PROP(PS_NODE, temperature_channel))
#endif

#define HAS_FAULT  DT_NODE_HAS_PROP(PS_NODE, fault_gpios)
#define HAS_OTW    DT_NODE_HAS_PROP(PS_NODE, otw_gpios)
#define HAS_OPAMPS DT_NODE_HAS_PROP(PS_NODE, opamps)
#define INVERT_B   DT_PROP(PS_NODE, phase_b_sense_invert)

/* Optional hardware overvoltage trip and offset window (hw_trip.h). No hardware
 * overcurrent trip: the gate driver limits and shuts down its own currents
 * (docs/protection.md).
 */
#define HAS_HW_OV         DT_NODE_HAS_PROP(PS_NODE, hw_trip_vbus_mv)
#define HAS_OFFSET_WINDOW DT_NODE_HAS_PROP(PS_NODE, current_offset_min_counts)
BUILD_ASSERT(HAS_OFFSET_WINDOW == DT_NODE_HAS_PROP(PS_NODE, current_offset_max_counts),
	     "current-offset-min-counts and current-offset-max-counts go together");
BUILD_ASSERT(HAS_HW_OV == DT_NODE_HAS_PROP(PS_NODE, hw_trip_vbus_comparator),
	     "hw-trip-vbus-mv and hw-trip-vbus-comparator go together");

/* Fast-loop interrupt: end of the injected sequence of the master ADC. */
#define ADC_IRQN     DT_IRQN(ADCM_NODE)
#define ADC_IRQ_PRIO 0

PINCTRL_DT_DEFINE(PS_NODE);

static const struct gpio_dt_spec resets[] = {
	GPIO_DT_SPEC_GET_BY_IDX(PS_NODE, reset_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(PS_NODE, reset_gpios, 1),
};
#if HAS_FAULT
static const struct gpio_dt_spec fault_pin = GPIO_DT_SPEC_GET(PS_NODE, fault_gpios);
#endif
#if HAS_OTW
static const struct gpio_dt_spec otw_pin = GPIO_DT_SPEC_GET(PS_NODE, otw_gpios);
#endif

#if HAS_OPAMPS
#define OPAMP_DEV(node, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node, prop, idx))
static const struct device *const opamps[] = {
	DT_FOREACH_PROP_ELEM_SEP(PS_NODE, opamps, OPAMP_DEV, (,))};
#endif

#if HAS_HW_OV
STM32_PA_TRIP_COMP_OKAY(PS_NODE, hw_trip_vbus_comparator, 0)
static const struct stm32_pa_trip_comp ov_comp =
	STM32_PA_TRIP_COMP_BY_IDX(PS_NODE, hw_trip_vbus_comparator, 0);
#endif

static const struct stm32_pclken tim_clk[] = STM32_DT_CLOCKS(TIM_NODE);
static const struct stm32_pclken adc_clk[] = STM32_DT_CLOCKS(ADC_COMMON_NODE);

static struct power_stage_info info;

/* Shared with the zero-latency ISR: written before the interrupt is enabled
 * or as single aligned words.
 */
static volatile power_stage_fast_cb_t fast_fn;
static void *volatile fast_ctx;
static volatile uint16_t offset_a = ADC_MID;
static volatile uint16_t offset_b = ADC_MID;
static volatile bool offsets_valid;       /* the leg trip means nothing before calibration */
static volatile uint16_t leg_trip_counts; /* 0 = disabled */
static volatile uint32_t isr_count;
static volatile uint16_t raw_temp; /* last NTC reading, 0 = none yet */
static volatile uint32_t fast_cycles_max;
static volatile bool outputs_on;
static volatile bool hw_trip_latched; /* break input or gate driver fault: off until reset */

/* Overrun of the fast loop: counted, reported to the callback once */
static volatile uint32_t overruns;
static volatile bool overrun_pending;

/* Offset calibration, accumulated by the ISR */
static volatile uint32_t cal_remaining;
static volatile uint32_t cal_sum_a;
static volatile uint32_t cal_sum_b;

/* ------------------------------------------------------------------------ */

static uint16_t recentre(uint16_t raw, uint16_t offset)
{
	const int32_t v = (int32_t)raw - (int32_t)offset + (int32_t)ADC_MID;

	if (v < 0) {
		return 0U;
	}
	return (v > (int32_t)ADC_FULL) ? (uint16_t)ADC_FULL : (uint16_t)v;
}

/* The same coil current with the other sign: around ADC_MID, kept in range */
static uint16_t mirror(uint16_t centred)
{
	const int32_t v = 2 * (int32_t)ADC_MID - (int32_t)centred;

	return (v > (int32_t)ADC_FULL) ? (uint16_t)ADC_FULL : (uint16_t)v;
}

/*
 * Both bridges high impedance, from any context: the GPIO writes are single stores to
 * BSRR/BRR (gpio_stm32.c), no kernel call, so the zero-latency interrupt may call this.
 */
static void bridges_off(void)
{
	(void)gpio_pin_set_dt(&resets[0], 1);
	(void)gpio_pin_set_dt(&resets[1], 1);
	LL_TIM_DisableAllOutputs(TIMx);
	outputs_on = false;
}

ISR_DIRECT_DECLARE(power_stage_adc_isr)
{
	if (LL_ADC_IsActiveFlag_JEOS(ADC_M) != 0U) {
		/* The slave sequence is shorter; wait for it only if it is late */
		for (uint32_t i = 0U; (i < 64U) && (LL_ADC_IsActiveFlag_JEOS(ADC_S) == 0U); i++) {
		}
		const uint16_t raw_a = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_1);
		const uint16_t raw_vbus = LL_ADC_INJ_ReadConversionData12(ADC_M, LL_ADC_INJ_RANK_2);
		const uint16_t raw_b = LL_ADC_INJ_ReadConversionData12(ADC_S, LL_ADC_INJ_RANK_1);
#if HAS_NTC
		raw_temp = LL_ADC_INJ_ReadConversionData12(ADC_S, LL_ADC_INJ_RANK_2);
#endif
		const power_stage_fast_cb_t fn = fast_fn;

		LL_ADC_ClearFlag_JEOS(ADC_M);
		LL_ADC_ClearFlag_JEOS(ADC_S);
		isr_count++;

		if (cal_remaining > 0U) {
			cal_sum_a += raw_a;
			cal_sum_b += raw_b;
			cal_remaining--;
		} else if (fn != NULL) {
			const uint32_t start = stm32_pa_cycles();
			const uint16_t i_b = recentre(raw_b, offset_b);
			const struct power_stage_sample sample = {
				.i_a = recentre(raw_a, offset_a),
				.i_b = INVERT_B ? mirror(i_b) : i_b,
				.vbus = raw_vbus,
				/* both legs of a bridge carry its coil current */
				.i_aux = (uint16_t)ADC_MID,
			};
			const int32_t trip = (int32_t)leg_trip_counts;
			uint32_t trips = 0U;

			/* Every coil, also in voltage drive where Stepper-C does not check */
			if ((trip != 0) && offsets_valid &&
			    ((abs((int32_t)raw_a - (int32_t)offset_a) > trip) ||
			     (abs((int32_t)raw_b - (int32_t)offset_b) > trip))) {
				trips |= POWER_STAGE_TRIP_AUX;
			}
			if (overrun_pending) {
				overrun_pending = false;
				trips |= POWER_STAGE_TRIP_OVERRUN;
			}
#if HAS_FAULT
			/* Only while the outputs are on: with the resets asserted, or with
			 * no bus voltage (undervoltage of GVDD), FAULT says nothing new
			 */
			if (outputs_on && (gpio_pin_get_dt(&fault_pin) > 0)) {
				bridges_off();
				hw_trip_latched = true;
				trips |= POWER_STAGE_TRIP_DRIVER;
			}
#endif
#if HAS_HW_OV
			if (LL_TIM_IsActiveFlag_BRK2(TIMx) != 0U) {
				LL_TIM_ClearFlag_BRK2(TIMx);
				/* BRK2 cleared MOE: the PWM inputs are low, the low sides on */
				bridges_off();
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
				bridges_off();
				overruns++;
				overrun_pending = true;
			}
		}
	}

	return 0; /* zero-latency interrupt: no rescheduling */
}

/* ------------------------------------------------------------------------ */

/* Resets asserted before anything else, so that the bridges stay open while the
 * timer and its pins come up
 */
static int gate_driver_gpios_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(resets); i++) {
		if (!gpio_is_ready_dt(&resets[i])) {
			return -ENODEV;
		}
		int ret = gpio_pin_configure_dt(&resets[i], GPIO_OUTPUT_ACTIVE);

		if (ret != 0) {
			LOG_ERR("reset pin %u (%d)", (unsigned int)i, ret);
			return ret;
		}
	}
#if HAS_FAULT
	if (!gpio_is_ready_dt(&fault_pin) || (gpio_pin_configure_dt(&fault_pin, GPIO_INPUT) != 0)) {
		return -ENODEV;
	}
#endif
#if HAS_OTW
	if (!gpio_is_ready_dt(&otw_pin) || (gpio_pin_configure_dt(&otw_pin, GPIO_INPUT) != 0)) {
		return -ENODEV;
	}
#endif
	return 0;
}

static int opamps_init(void)
{
#if HAS_OPAMPS
	for (size_t i = 0; i < ARRAY_SIZE(opamps); i++) {
		if (!device_is_ready(opamps[i])) {
			LOG_ERR("%s not ready", opamps[i]->name);
			return -ENODEV;
		}
#if DT_NODE_HAS_PROP(PS_NODE, opamp_gain)
		int ret = opamp_set_gain(opamps[i], DT_STRING_TOKEN(PS_NODE, opamp_gain));

		if (ret != 0) {
			LOG_ERR("%s: gain (%d)", opamps[i]->name, ret);
			return ret;
		}
#endif
	}
#endif
	return 0;
}

static void timer_init(void)
{
	TIM_TypeDef *const tim = TIMx;
	const uint32_t arr = info.timer_clock_hz / (2U * info.pwm_freq_hz);
	static const uint32_t legs[] = {LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH2, LL_TIM_CHANNEL_CH3,
					LL_TIM_CHANNEL_CH4};

	info.pwm_period_ticks = (uint16_t)arr;
	/* Compare ticks = timer ticks: a leg at CCR = ARR - W is low for W ticks on each
	 * side of the top of the counter
	 */
	info.sample_window_ticks = (uint16_t)MIN(
		stm32_pa_ns_to_ticks(DT_PROP(PS_NODE, sample_window_ns), info.timer_clock_hz),
		arr / 4U);

	stm32_pa_timer_base(tim, arr);
	for (size_t i = 0; i < ARRAY_SIZE(legs); i++) {
		/* one PWM input per leg: the gate driver makes the low-side signal */
		stm32_pa_timer_leg(tim, legs[i], 0U, arr);
	}
	(void)stm32_pa_timer_adc_trigger(tim, TRIGGER_CHANNEL, arr);

	/* With MOE = 0 the PWM inputs are driven to their idle level (low); the resets
	 * keep the bridges open meanwhile. No dead time in the timer.
	 */
	LL_TIM_SetOffStates(tim, LL_TIM_OSSI_ENABLE, LL_TIM_OSSR_ENABLE);
	LL_TIM_OC_SetDeadTime(tim, 0U);

	LL_TIM_CC_EnableChannel(tim, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2 | LL_TIM_CHANNEL_CH3 |
					     LL_TIM_CHANNEL_CH4);

	stm32_pa_timer_start(tim);

	LOG_INF("TIM1: %u Hz clock, ARR %u (%u Hz), gate driver dead time %u ns, ADC trigger "
		"CH%u, sample window %u ticks",
		info.timer_clock_hz, arr, info.pwm_freq_hz, info.dead_time_ns,
		DT_PROP(PS_NODE, adc_trigger_channel), info.sample_window_ticks);
}

#if HAS_HW_OV
static int hw_trips_init(void)
{
	int ret = stm32_pa_trip_comp_init(&ov_comp);

	if (ret != 0) {
		return ret;
	}
	k_busy_wait(10); /* comparator start-up, DAC settling */
	stm32_pa_break_init(TIMx, 0U, stm32_pa_break_source(ov_comp.regs));
	return 0;
}

/* Level from the scaling of the bus divider */
static void hw_trips_arm(void)
{
	const uint32_t trip_mv = DT_PROP(PS_NODE, hw_trip_vbus_mv);
	const uint16_t dac_vbus = hw_trip_vbus_dac(trip_mv, info.adc_vbus_mv_full, ADC_FULL);

	(void)dac_write_value(ov_comp.dac, ov_comp.dac_channel, dac_vbus);
	LOG_INF("hardware overvoltage trip %u mV: DAC level %u", trip_mv, dac_vbus);
}
#endif /* HAS_HW_OV */

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

	/* Master: current A, then the divider (~9 kOhm), which needs more sampling time.
	 * Slave: current B, then the NTC (5 kOhm and 10 nF): a shorter sequence than the
	 * master, so that both are done when the master interrupt comes.
	 */
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_A, LL_ADC_SAMPLINGTIME_6CYCLES_5);
	LL_ADC_SetChannelSamplingTime(ADC_M, CH_VBUS, LL_ADC_SAMPLINGTIME_24CYCLES_5);
	LL_ADC_SetChannelSamplingTime(ADC_S, CH_B, LL_ADC_SAMPLINGTIME_6CYCLES_5);

	LL_ADC_INJ_ConfigQueueContext(ADC_M, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_ENABLE_2RANKS,
				      CH_A, CH_VBUS, CH_A, CH_A);
#if HAS_NTC
	LL_ADC_SetChannelSamplingTime(ADC_S, CH_TEMP, LL_ADC_SAMPLINGTIME_12CYCLES_5);
	LL_ADC_INJ_ConfigQueueContext(ADC_S, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_ENABLE_2RANKS,
				      CH_B, CH_TEMP, CH_B, CH_B);
#else
	LL_ADC_INJ_ConfigQueueContext(ADC_S, LL_ADC_INJ_TRIG_EXT_TIM1_TRGO2,
				      LL_ADC_INJ_TRIG_EXT_RISING, LL_ADC_INJ_SEQ_SCAN_DISABLE, CH_B,
				      CH_B, CH_B, CH_B);
#endif

	LL_ADC_ClearFlag_JEOS(ADC_M);
	LL_ADC_ClearFlag_JEOS(ADC_S);
	LL_ADC_EnableIT_JEOS(ADC_M);

	LL_ADC_INJ_StartConversion(ADC_S);
	LL_ADC_INJ_StartConversion(ADC_M);
	return 0;
}

static void scaling_init(void)
{
	info.topology = POWER_STAGE_FULL_BRIDGE;
	info.legs = 4U;
	info.pwm_freq_hz = DT_PROP(PS_NODE, pwm_frequency);
	info.dead_time_ns = DT_PROP(PS_NODE, dead_time_ns);
	/* The gate driver prevents shoot-through itself */
	info.outputs_allowed = true;
	power_stage_scale(
		&info, ADC_BITS, DT_PROP(PS_NODE, adc_vref_mv), DT_PROP(PS_NODE, shunt_micro_ohms),
		DT_PROP(PS_NODE, current_sense_gain_milli),
		DT_PROP_BY_IDX(PS_NODE, vbus_divider, 0), DT_PROP_BY_IDX(PS_NODE, vbus_divider, 1),
		DT_PROP(PS_NODE, current_sense_invert));
}

static int fb_init(const struct device *dev)
{
	int ret;

	ARG_UNUSED(dev);

	ret = gate_driver_gpios_init();
	if (ret != 0) {
		return ret;
	}

	scaling_init();

	ret = stm32_pa_clocks_on(tim_clk, adc_clk, &info.timer_clock_hz);
	if (ret != 0) {
		return ret;
	}

	ret = opamps_init();
	if (ret != 0) {
		return ret;
	}

	/* Timer first, with MOE = 0: when the pins switch to TIM1 they are already
	 * driven to the idle level; the resets keep the bridges open anyway.
	 */
	timer_init();
#if HAS_HW_OV
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

	LOG_INF("current full scale %u mA (span %d), VBUS full scale %u mV", info.i_full_scale_ma,
		info.adc_i_span, info.adc_vbus_mv_full);
	return 0;
}

static int fb_get_info(const struct device *dev, struct power_stage_info *out)
{
	ARG_UNUSED(dev);
	*out = info;
	/* Not before a successful calibration, never after a latched trip */
	out->outputs_allowed = info.outputs_allowed && offsets_valid && !hw_trip_latched;
	return 0;
}

static int fb_calibrate(const struct device *dev, uint32_t samples)
{
	const int64_t deadline = k_uptime_get() + 1000 + (int64_t)samples / 10;

	ARG_UNUSED(dev);

	if ((samples == 0U) || outputs_on) {
		return -EINVAL;
	}

	offsets_valid = false;
	cal_sum_a = 0U;
	cal_sum_b = 0U;
	cal_remaining = samples; /* the ISR starts accumulating */

	while (cal_remaining > 0U) {
		if (k_uptime_get() > deadline) {
			cal_remaining = 0U;
			LOG_ERR("offset calibration timed out (no ADC interrupts?)");
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}

	offset_a = (uint16_t)((cal_sum_a + samples / 2U) / samples);
	offset_b = (uint16_t)((cal_sum_b + samples / 2U) / samples);
	LOG_INF("current offsets A %u, B %u counts", offset_a, offset_b);
#if HAS_OFFSET_WINDOW
	{
		const uint16_t lo = DT_PROP(PS_NODE, current_offset_min_counts);
		const uint16_t hi = DT_PROP(PS_NODE, current_offset_max_counts);

		if (!hw_offset_in_window(offset_a, lo, hi) ||
		    !hw_offset_in_window(offset_b, lo, hi)) {
			LOG_ERR("current offsets outside %u..%u: amplifier, shunt or bias fault; "
				"outputs refused",
				lo, hi);
			return -EIO;
		}
	}
#endif
#if HAS_HW_OV
	hw_trips_arm();
#endif
	offsets_valid = true;
	return 0;
}

static int fb_set_fast_callback(const struct device *dev, power_stage_fast_cb_t cb, void *ctx)
{
	ARG_UNUSED(dev);
	fast_fn = NULL;
	fast_ctx = ctx;
	fast_fn = cb;
	return 0;
}

static int fb_set_aux_trip(const struct device *dev, uint32_t trip_ma)
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

static int fb_write(const struct device *dev, const uint16_t *compare, size_t legs)
{
	TIM_TypeDef *const tim = TIMx;

	ARG_UNUSED(dev);

	if (legs < 4U) {
		return -EINVAL;
	}
	LL_TIM_OC_SetCompareCH1(tim, compare[0]);
	LL_TIM_OC_SetCompareCH2(tim, compare[1]);
	LL_TIM_OC_SetCompareCH3(tim, compare[2]);
	LL_TIM_OC_SetCompareCH4(tim, compare[3]);
	return 0;
}

static int fb_output_enable(const struct device *dev, bool enable)
{
	ARG_UNUSED(dev);

	if (!enable) {
		bridges_off();
		return 0;
	}
	if (!info.outputs_allowed || !offsets_valid) {
		return -EPERM;
	}
	if (hw_trip_latched) {
		return -EIO;
	}
	/* PWM inputs first, then the bridges: they start on the compares of this period */
	LL_TIM_EnableAllOutputs(TIMx);
	outputs_on = true;
	(void)gpio_pin_set_dt(&resets[0], 0);
	(void)gpio_pin_set_dt(&resets[1], 0);
	return 0;
}

static int fb_get_stats(const struct device *dev, struct power_stage_stats *stats)
{
	ARG_UNUSED(dev);
	stats->isr_count = isr_count;
	stats->fast_cycles_max = fast_cycles_max;
	stats->overruns = overruns;
	fast_cycles_max = 0U;
	return 0;
}

static int fb_get_temperature(const struct device *dev, int16_t *decidegc)
{
	int ret = -ENOSYS;

	ARG_UNUSED(dev);
#if HAS_NTC
	const uint16_t counts = raw_temp;

	if (counts == 0U) {
		return -EAGAIN; /* no conversion yet */
	}
	ret = ntc_decidegc(counts, ADC_FULL, DT_PROP(PS_NODE, ntc_r25_ohm),
			   DT_PROP(PS_NODE, ntc_beta), DT_PROP(PS_NODE, ntc_bottom_ohm), decidegc);
#endif
#if HAS_OTW
	/* The die of the gate driver is hotter than the board around the NTC */
	if (gpio_pin_get_dt(&otw_pin) > 0) {
		const int16_t otw = (int16_t)(DT_PROP(PS_NODE, otw_temperature_celsius) * 10);

		if ((ret != 0) || (*decidegc < otw)) {
			*decidegc = otw;
		}
		ret = 0;
	}
#endif
	ARG_UNUSED(decidegc);
	return ret;
}

static DEVICE_API(power_stage, fb_api) = {
	.get_info = fb_get_info,
	.calibrate = fb_calibrate,
	.set_fast_callback = fb_set_fast_callback,
	.write = fb_write,
	.output_enable = fb_output_enable,
	.set_aux_trip = fb_set_aux_trip,
	.get_stats = fb_get_stats,
	.get_temperature = fb_get_temperature,
};

DEVICE_DT_INST_DEFINE(0, fb_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_POWER_STAGE_INIT_PRIORITY,
		      &fb_api);
