/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power stage driver class: PWM legs, phase current and bus voltage measurement
 * synchronised with the PWM, and the fast-loop interrupt.
 *
 * Zephyr's PWM and ADC APIs have no dead time, center alignment, complementary
 * outputs or timer-triggered conversions, so a current loop needs MCU-specific
 * code. It lives in one backend per MCU family, selected by the devicetree. Nothing
 * above this API depends on the MCU, on an FPU or on zero-latency interrupts.
 */

#ifndef DOPPELSTEP_DRIVERS_POWER_STAGE_H_
#define DOPPELSTEP_DRIVERS_POWER_STAGE_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest number of PWM legs of any topology. */
#define POWER_STAGE_LEGS_MAX 4U

enum power_stage_topology {
	POWER_STAGE_SHARED_LEG,  /**< Three half-bridges, one shared by both phases. */
	POWER_STAGE_FULL_BRIDGE, /**< Two full H-bridges. */
};

/** Topology and scaling of a power stage. */
struct power_stage_info {
	enum power_stage_topology topology;
	uint8_t legs;              /**< Number of PWM legs. */
	bool outputs_allowed;      /**< false: outputs refused, for example no dead time set. */
	uint32_t timer_clock_hz;   /**< PWM timer clock [Hz]. */
	uint16_t pwm_period_ticks; /**< Compare value for 100 % duty. */
	uint32_t pwm_freq_hz;      /**< PWM frequency [Hz]. */
	uint32_t dead_time_ns;     /**< Dead time [ns]; 0 = not set. */
	uint32_t i_full_scale_ma;  /**< Phase current for |adc_i_span| counts [mA]. */
	int32_t adc_i_span;        /**< Counts from 0 A to +i_full_scale_ma (signed). */
	uint16_t adc_i_zero;       /**< Reading at 0 A after offset compensation. */
	uint32_t adc_vbus_mv_full; /**< Bus voltage at adc_full [mV]. */
	uint16_t adc_full;         /**< Largest ADC reading. */
};

/** One synchronous measurement, in ADC counts. */
struct power_stage_sample {
	uint16_t i_a;  /**< Phase A current, zero at adc_i_zero. */
	uint16_t i_b;  /**< Phase B current, zero at adc_i_zero. */
	uint16_t vbus; /**< Bus voltage. */
};

/** Diagnostics of the fast loop. */
struct power_stage_stats {
	uint32_t isr_count;       /**< Fast-loop periods since start. */
	uint32_t fast_cycles_max; /**< Longest callback since the last read [CPU cycles]; 0 = unknown. */
};

/**
 * @brief Fast-loop callback, at the PWM rate in the context of the backend's
 *        interrupt, which may run above the kernel's interrupt lock.
 *        It must not call kernel services.
 * @param sample    Measurement of this period.
 * @param aux_trip  true when a leg that the sample does not show exceeds the trip level.
 *                  Always false until power_stage_calibrate() has succeeded: without the
 *                  sensor offset the level means nothing, and a fault would latch.
 * @param ctx       Pointer given to power_stage_set_fast_callback().
 */
typedef void (*power_stage_fast_cb_t)(const struct power_stage_sample *sample, bool aux_trip,
				      void *ctx);

__subsystem struct power_stage_driver_api {
	int (*get_info)(const struct device *dev, struct power_stage_info *info);
	int (*calibrate)(const struct device *dev, uint32_t samples);
	int (*set_fast_callback)(const struct device *dev, power_stage_fast_cb_t cb, void *ctx);
	int (*write)(const struct device *dev, const uint16_t *compare, size_t legs);
	int (*output_enable)(const struct device *dev, bool enable);
	/* Optional: */
	int (*set_aux_trip)(const struct device *dev, uint32_t trip_ma);
	int (*get_stats)(const struct device *dev, struct power_stage_stats *stats);
	int (*position_shifted)(const struct device *dev, int32_t delta_usteps);
};

/** Topology and scaling. */
static inline int power_stage_get_info(const struct device *dev, struct power_stage_info *info)
{
	return DEVICE_API_GET(power_stage, dev)->get_info(dev, info);
}

/** Measure the current sensor offsets with the outputs off. Blocks the caller. */
static inline int power_stage_calibrate(const struct device *dev, uint32_t samples)
{
	return DEVICE_API_GET(power_stage, dev)->calibrate(dev, samples);
}

/** Install the fast-loop callback; NULL removes it. */
static inline int power_stage_set_fast_callback(const struct device *dev,
						power_stage_fast_cb_t cb, void *ctx)
{
	return DEVICE_API_GET(power_stage, dev)->set_fast_callback(dev, cb, ctx);
}

/** Compare values of the legs, for the next PWM period. Allowed in the fast callback. */
static inline int power_stage_write(const struct device *dev, const uint16_t *compare, size_t legs)
{
	return DEVICE_API_GET(power_stage, dev)->write(dev, compare, legs);
}

/** Switch the outputs. Allowed in the fast callback; -EPERM if outputs are not allowed. */
static inline int power_stage_output_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(power_stage, dev)->output_enable(dev, enable);
}

/** Overcurrent level of the leg the sample does not show [mA]; 0 disables the check. */
static inline int power_stage_set_aux_trip(const struct device *dev, uint32_t trip_ma)
{
	const struct power_stage_driver_api *api = DEVICE_API_GET(power_stage, dev);

	return (api->set_aux_trip != NULL) ? api->set_aux_trip(dev, trip_ma) : -ENOSYS;
}

/** Diagnostics; reading clears fast_cycles_max. */
static inline int power_stage_get_stats(const struct device *dev, struct power_stage_stats *stats)
{
	const struct power_stage_driver_api *api = DEVICE_API_GET(power_stage, dev);

	return (api->get_stats != NULL) ? api->get_stats(dev, stats) : -ENOSYS;
}

/**
 * The position coordinate of the motor changed by @p delta_usteps; the rotor did not
 * move. Only a backend that models the rotor needs to know.
 */
static inline int power_stage_position_shifted(const struct device *dev, int32_t delta_usteps)
{
	const struct power_stage_driver_api *api = DEVICE_API_GET(power_stage, dev);

	return (api->position_shifted != NULL) ? api->position_shifted(dev, delta_usteps) : -ENOSYS;
}

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_DRIVERS_POWER_STAGE_H_ */
