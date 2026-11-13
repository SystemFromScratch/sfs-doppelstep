/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Measurement variables and a capture buffer of the fast loop, for MCUViewer or any
 * other tool that reads the RAM over SWD (CONFIG_SSTEP_MOTOR_MCUVIEWER). Without the
 * option the functions are empty and the variables do not exist.
 *
 * The probe records what the driver sees anyway: the ADC samples that go into
 * Stepper-C and the compare values that come out of it. Measurements: the current
 * ripple at rest (hardware checklist, check 15) and the effective dead time from the
 * phase voltage that the current loop needs at a given current (docs/measurements.md).
 */

#ifndef SSTEP_MCUVIEWER_H
#define SSTEP_MCUVIEWER_H

#include <stdbool.h>
#include <stdint.h>

#include <doppelstep/drivers/power_stage.h>

#if defined(CONFIG_SSTEP_MOTOR_MCUVIEWER)

/** Layout version of struct sstep_mcuviewer; tools/mcuviewer/mcuv.py checks it. */
#define SSTEP_MCUV_VERSION 1U
#define SSTEP_MCUV_MAGIC   0x5655434DU /* "MCUV" in memory (little endian) */

/** One recorded fast-loop period. */
struct sstep_mcuv_point {
	int16_t i_a_ma; /**< Measured phase A current [mA]. */
	int16_t i_b_ma; /**< Measured phase B current [mA]. */
	int16_t v_a_mv; /**< Phase A voltage commanded for the sampled period [mV]. */
	int16_t v_b_mv; /**< Phase B voltage commanded for the sampled period [mV]. */
};

/**
 * Everything MCUViewer shows, in one variable `mcuv` of the ELF file. Offsets are
 * fixed (checked at build time): tools/mcuviewer/mcuv.py reads them without DWARF.
 */
struct sstep_mcuviewer {
	/* Header, written once at init */
	uint32_t magic;          /**< SSTEP_MCUV_MAGIC once the probe is attached. */
	uint32_t version;        /**< SSTEP_MCUV_VERSION. */
	uint32_t pwm_freq_hz;    /**< Rate of the fast loop [Hz]. */
	uint32_t dead_time_ns;   /**< Dead time set in the devicetree [ns]. */
	uint32_t shared_leg;     /**< 1: phases A = U - W, B = V - W; 0: A = U1 - U2, B = V1 - V2. */

	/* Live values, every fast-loop period */
	uint32_t periods;        /**< Fast-loop periods since attach. */
	int32_t i_a_ma;          /**< Measured phase A current [mA]. */
	int32_t i_b_ma;          /**< Measured phase B current [mA]. */
	int32_t i_w_ma;          /**< Current of the shared leg, 0 when not measured [mA]. */
	int32_t v_a_mv;          /**< Phase A voltage commanded for the sampled period [mV]. */
	int32_t v_b_mv;          /**< Phase B voltage commanded for the sampled period [mV]. */
	int32_t vbus_mv;         /**< Bus voltage [mV]. */
	uint32_t outputs_on;     /**< 1 while the bridge switches. */

	/* Averages over about 2^SSTEP_MCUV_AVG_SHIFT periods (first-order filter) */
	int32_t i_a_avg_ma;
	int32_t i_b_avg_ma;
	int32_t v_a_avg_mv;
	int32_t v_b_avg_mv;

	/* Peak-to-peak of the measured current over the last window (check 15) */
	uint32_t window_periods; /**< Window length: 100 ms. */
	int32_t i_a_pp_ma;
	int32_t i_b_pp_ma;

	/* Capture: write capture_arm = 1, the fast loop fills mcuv_buf */
	uint32_t capture_arm;     /**< Write 1 to start; 0 again when the buffer is full. */
	uint32_t capture_divider; /**< Record every n-th period; 0 or 1 = every period. */
	uint32_t capture_count;   /**< Points recorded by the last or the running capture. */
	uint32_t capture_length;  /**< Capacity of mcuv_buf. */
	uint32_t capture_rate_hz; /**< Rate of the last capture: pwm_freq_hz / divider. */
};

/** The variables of the probe, for the debugger and MCUViewer. */
extern volatile struct sstep_mcuviewer mcuv;
extern volatile struct sstep_mcuv_point mcuv_buf[];

/**
 * Attach the probe to one motor instance. The first caller owns it; later callers
 * get false and are not recorded.
 */
bool sstep_mcuv_attach(const struct power_stage_info *info);

/** Compare values just written to the legs (from the pwm_write callback). */
void sstep_mcuv_compare(const uint16_t *compare, uint8_t legs);

/** One fast-loop sample; outputs_on: the bridge switched during the sampled period. */
void sstep_mcuv_sample(const struct power_stage_sample *s, bool outputs_on);

#else

static inline bool sstep_mcuv_attach(const struct power_stage_info *info)
{
	(void)info;
	return false;
}

static inline void sstep_mcuv_compare(const uint16_t *compare, uint8_t legs)
{
	(void)compare;
	(void)legs;
}

static inline void sstep_mcuv_sample(const struct power_stage_sample *s, bool outputs_on)
{
	(void)s;
	(void)outputs_on;
}

#endif /* CONFIG_SSTEP_MOTOR_MCUVIEWER */

#endif /* SSTEP_MCUVIEWER_H */
