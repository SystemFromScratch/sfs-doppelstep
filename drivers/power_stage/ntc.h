/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NTC thermistor in a divider, for the power stage backends (floating point, which
 * the layers above the backends do not use: scripts/check-layers.sh).
 *
 *   Vref -- NTC -- ADC input -- R_bottom -- GND
 *
 *   counts / full = R_bottom / (R_ntc + R_bottom)  =>  R_ntc = R_bottom (full - counts) / counts
 *   1 / T = 1 / T25 + ln(R_ntc / R25) / B          (beta model, T in kelvin)
 */

#ifndef DOPPELSTEP_POWER_STAGE_NTC_H_
#define DOPPELSTEP_POWER_STAGE_NTC_H_

#include <errno.h>
#include <math.h>
#include <stdint.h>

/* Readings this close to a rail mean an open or shorted NTC, not a temperature: with the
 * ESC1 values below -60 degC or far above the 125 degC the NTC is rated for.
 */
#define NTC_RAIL_COUNTS 20U

/**
 * @param counts    ADC reading of the divider.
 * @param full      Largest ADC reading.
 * @param r25       NTC resistance at 25 degC [Ohm].
 * @param beta      B constant [K].
 * @param r_bottom  Resistor from the input to ground [Ohm].
 * @param decidegc  Temperature [0.1 degC].
 * @retval -EIO the reading is at a rail.
 */
static inline int ntc_decidegc(uint32_t counts, uint32_t full, uint32_t r25, uint32_t beta,
			       uint32_t r_bottom, int16_t *decidegc)
{
	if ((counts < NTC_RAIL_COUNTS) || (counts > (full - NTC_RAIL_COUNTS))) {
		return -EIO;
	}
	const float r_ntc = (float)r_bottom * (float)(full - counts) / (float)counts;
	const float inv_t = (1.0f / 298.15f) + (logf(r_ntc / (float)r25) / (float)beta);
	const float t_c = (1.0f / inv_t) - 273.15f;

	*decidegc = (int16_t)lroundf(t_c * 10.0f);
	return 0;
}

#endif /* DOPPELSTEP_POWER_STAGE_NTC_H_ */
