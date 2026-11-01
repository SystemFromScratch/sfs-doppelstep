/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated motor behind the power stage (CONFIG_SFS_SIM_PLANT): averaged
 * shared-leg power stage, two-phase hybrid stepper and current sensing, the
 * model of sfs-stepper-sim (sstep_xcos_plant). The power stage calls it from
 * the fast-loop interrupt in place of the ADC readings and the gate drivers:
 *
 *   sim_plant_sample()      ADC counts of the state at the sampling instant
 *   fast loop (Stepper-C)   -> power_stage_pwm_write() -> sim_plant_set_compare()
 *   sim_plant_step()        integrate one PWM period with the voltage of the
 *                           previous compare values (one-period delay, as on
 *                           the hardware), then take the new ones
 */

#ifndef SFS_SIM_PLANT_H
#define SFS_SIM_PLANT_H

#include <stdbool.h>
#include <stdint.h>

#include <sstep.h>

#include "power_stage.h"

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
	int32_t rotor_usteps; /**< Rotor position [microsteps], same zero as Stepper-C. */
	int32_t load_unm;     /**< Load torque [uN*m]. */
	bool enabled;         /**< Outputs enabled (windings driven). */
};

/** Set up the model with the scaling of the power stage; rotor at rest at angle 0. */
int sim_plant_init(const struct power_stage_info *info);

/** ADC sample of the present state and |iA + iB| of the shared leg [mA]. ISR. */
void sim_plant_sample(struct sstep_sample *sample, uint32_t *i_w_ma);

/** Compare values written by Stepper-C for the next period. ISR. */
void sim_plant_set_compare(const uint16_t compare[SSTEP_LEGS_MAX]);

/** Outputs on (windings driven) or off (windings open). Any context. */
void sim_plant_set_enabled(bool enable);

/** Advance the model by one PWM period. ISR, after the fast loop. */
void sim_plant_step(void);

/** Load torque [uN*m], positive against forward motion. Any context. */
void sim_plant_set_load(int32_t load_unm);

/** Follow sstep_set_position(): shift the reported rotor position. Any context. */
void sim_plant_shift_position(int32_t delta_usteps);

/** Copy of the latest published state (thread context). */
void sim_plant_get_state(struct sim_plant_state *state);

#ifdef __cplusplus
}
#endif

#endif /* SFS_SIM_PLANT_H */
