/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Levels of the hardware trip comparators, in DAC codes of the same reference as
 * the ADC (both VREF+), and the check of the current sensor offsets. Integer math,
 * so that the host tests cover it.
 *
 * Overcurrent: the comparator watches the non-inverting input of the current
 * amplifier, where the shunt voltage sits on the bias of the board; the amplifier
 * multiplies both by its PGA gain. So the input at the trip current is
 * (offset + trip counts at the amplifier output) / gain.
 */

#ifndef DOPPELSTEP_POWER_STAGE_HW_TRIP_H_
#define DOPPELSTEP_POWER_STAGE_HW_TRIP_H_

#include <stdbool.h>
#include <stdint.h>

#define HW_TRIP_DAC_FULL 4095U
#define HW_TRIP_ADC_HALF 2048U /* counts from 0 A to the full-scale current */

/**
 * @param offset_counts   Amplifier output at 0 A (calibrated offset) [ADC counts].
 * @param trip_ma         Trip current [mA].
 * @param i_full_scale_ma Current for HW_TRIP_ADC_HALF counts at the amplifier output [mA].
 * @param pga_gain        Gain of the amplifier from its non-inverting input.
 * @return DAC code, rounded, at most HW_TRIP_DAC_FULL.
 */
static inline uint16_t hw_trip_oc_dac(uint16_t offset_counts, uint32_t trip_ma,
				      uint32_t i_full_scale_ma, uint32_t pga_gain)
{
	const uint64_t trip_counts = (uint64_t)trip_ma * HW_TRIP_ADC_HALF / i_full_scale_ma;
	const uint64_t code = ((uint64_t)offset_counts + trip_counts + pga_gain / 2U) / pga_gain;

	return (uint16_t)((code > HW_TRIP_DAC_FULL) ? HW_TRIP_DAC_FULL : code);
}

/**
 * @param trip_mv      Bus voltage of the trip [mV].
 * @param vbus_mv_full Bus voltage at adc_full [mV].
 * @param adc_full     Largest ADC reading.
 * @return DAC code, rounded, at most adc_full.
 */
static inline uint16_t hw_trip_vbus_dac(uint32_t trip_mv, uint32_t vbus_mv_full, uint16_t adc_full)
{
	const uint64_t code = ((uint64_t)trip_mv * adc_full + vbus_mv_full / 2U) / vbus_mv_full;

	return (uint16_t)((code > adc_full) ? adc_full : code);
}

/** An offset outside [min, max] means a broken amplifier, shunt or bias network. */
static inline bool hw_offset_in_window(uint16_t offset, uint16_t min, uint16_t max)
{
	return (offset >= min) && (offset <= max);
}

#endif /* DOPPELSTEP_POWER_STAGE_HW_TRIP_H_ */
