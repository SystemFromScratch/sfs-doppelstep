/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * B-G431B-ESC1 power stage: TIM1 complementary center-aligned PWM on three
 * legs, OPAMP current amplifiers, ADC1/ADC2 injected conversions triggered by
 * TIM1 at the center of the PWM period, and the fast-loop interrupt.
 * Hardware description: the "sfs,stm32-shared-leg-power-stage" devicetree node.
 */

#ifndef SFS_POWER_STAGE_H
#define SFS_POWER_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <sstep.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Scaling of the power stage, in the units of struct sstep_stage_cfg. */
struct power_stage_info {
	uint32_t timer_clock_hz;   /**< TIM1 kernel clock [Hz]. */
	uint16_t pwm_period_ticks; /**< Compare value for 100 % duty (ARR). */
	uint32_t pwm_freq_hz;      /**< PWM frequency [Hz]. */
	uint32_t dead_time_ns;     /**< Dead time [ns] from the devicetree; 0 = not set. */
	uint32_t i_full_scale_ma;  /**< Phase current for |adc_i_span| counts [mA]. */
	int32_t adc_i_span;        /**< Counts from 0 A to +i_full_scale_ma (signed). */
	uint16_t adc_i_zero;       /**< Reading at 0 A after offset compensation. */
	uint32_t adc_vbus_mv_full; /**< Bus voltage at adc_full [mV]. */
	uint16_t adc_full;         /**< Largest ADC reading. */
};

/**
 * @brief Fast-loop callback, called from the zero-latency ADC interrupt at the
 *        PWM rate. Must not call kernel services.
 * @param sample  Phase currents (offset-compensated, zero at adc_i_zero) and bus voltage.
 * @param w_trip  true when |iW| of the shared leg exceeds the trip level.
 */
typedef void (*power_stage_fast_fn)(const struct sstep_sample *sample, bool w_trip, void *ctx);

/** Configure OPAMPs, TIM1 and the ADCs; PWM runs, outputs stay off. */
int power_stage_init(void);

/** Scaling values for sstep_init(); valid after power_stage_init(). */
const struct power_stage_info *power_stage_get_info(void);

/** Measure the current sensor offsets with the outputs off (blocks the caller). */
int power_stage_calibrate(uint32_t samples);

/** Install the fast-loop callback (NULL removes it). */
void power_stage_set_fast_fn(power_stage_fast_fn fn, void *ctx);

/** Overcurrent level of the shared leg check [mA]; 0 disables the check. */
void power_stage_set_w_trip(uint32_t trip_ma);

/** Stepper-C pwm_write callback: compare values of legs U, V, W. */
int power_stage_pwm_write(void *context, const uint16_t compare[SSTEP_LEGS_MAX]);

/** Stepper-C output_enable callback: TIM1 main output enable (MOE). */
int power_stage_output_enable(void *context, bool enable);

/** Number of fast-loop interrupts since start (for diagnostics). */
uint32_t power_stage_isr_count(void);

/** Longest fast-loop callback since the last reset [CPU cycles] (DWT counter). */
uint32_t power_stage_fast_cycles_max(bool reset);

#ifdef __cplusplus
}
#endif

#endif /* SFS_POWER_STAGE_H */
