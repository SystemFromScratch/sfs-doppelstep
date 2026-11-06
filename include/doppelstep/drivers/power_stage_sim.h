/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Extension of the simulation backend (sfs,sim-power-stage) of the power_stage class.
 */

#ifndef DOPPELSTEP_DRIVERS_POWER_STAGE_SIM_H_
#define DOPPELSTEP_DRIVERS_POWER_STAGE_SIM_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** State of the model, published every PWM period. */
struct sim_plant_state {
	int32_t i_a_ma;       /**< Phase A current [mA]. */
	int32_t i_b_ma;       /**< Phase B current [mA]. */
	int32_t v_a_mv;       /**< Phase A voltage applied in the last period [mV]. */
	int32_t v_b_mv;       /**< Phase B voltage applied in the last period [mV]. */
	int32_t speed_mrpm;   /**< Rotor speed [0.001 rpm]. */
	int32_t rotor_usteps; /**< Rotor position [microsteps], same zero as the controller. */
	int32_t load_unm;     /**< Load torque [uN*m]. */
	bool enabled;         /**< Outputs enabled (windings driven). */
};

/**
 * Load torque [uN*m], positive against forward motion, acting from the first
 * time the outputs are enabled (at power-on the joint rests on a support).
 */
int power_stage_sim_set_load(const struct device *dev, int32_t load_unm);

/** Bus voltage of the model [mV]. */
int power_stage_sim_set_vbus(const struct device *dev, uint32_t vbus_mv);

/** Copy of the latest published state. Thread context. */
int power_stage_sim_get_state(const struct device *dev, struct sim_plant_state *state);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_DRIVERS_POWER_STAGE_SIM_H_ */
