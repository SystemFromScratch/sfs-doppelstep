/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scaling of a low-side shunt power stage from the properties of its node
 * (power-stage-common.yaml), shared by the backends.
 */

#ifndef DOPPELSTEP_POWER_STAGE_SCALING_H_
#define DOPPELSTEP_POWER_STAGE_SCALING_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <doppelstep/drivers/power_stage.h>

/**
 * @param adc_bits    ADC resolution.
 * @param vref_mv     ADC reference voltage [mV].
 * @param shunt_uohm  Shunt resistance [uOhm].
 * @param gain_milli  Voltage gain from the shunt to the ADC input, x1000, negative if inverting.
 * @param r_low       Lower resistor of the bus voltage divider [Ohm].
 * @param r_high      Upper resistor of the bus voltage divider [Ohm].
 * @param invert      Invert the measured current polarity (current-sense-invert).
 */
static inline void power_stage_scale(struct power_stage_info *info, uint32_t adc_bits,
				     uint32_t vref_mv, uint32_t shunt_uohm, int32_t gain_milli,
				     uint32_t r_low, uint32_t r_high, bool invert)
{
	const uint32_t adc_mid = 1UL << (adc_bits - 1U);
	const uint32_t adc_full = (1UL << adc_bits) - 1U;
	const uint32_t gain_abs = (uint32_t)abs(gain_milli);

	/* adc_mid counts = vref / 2 at the amplifier output:
	 * I [mA] = V [mV] / (R [Ohm] * G) = V [mV] * 1e9 / (R [uOhm] * G [x1000])
	 */
	info->i_full_scale_ma = (uint32_t)(((uint64_t)vref_mv * adc_mid / (adc_full + 1U)) *
					   1000000000ULL / ((uint64_t)shunt_uohm * gain_abs));
	info->adc_i_span = (gain_milli < 0) ? -(int32_t)adc_mid : (int32_t)adc_mid;
	if (invert) {
		info->adc_i_span = -info->adc_i_span;
	}
	info->adc_i_zero = (uint16_t)adc_mid;
	info->adc_vbus_mv_full = (uint32_t)((uint64_t)vref_mv * (r_low + r_high) / r_low);
	info->adc_full = (uint16_t)adc_full;
}

#endif /* DOPPELSTEP_POWER_STAGE_SCALING_H_ */
