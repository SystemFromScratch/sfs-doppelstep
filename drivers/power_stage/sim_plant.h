/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr adapter of the plant model of Stepper-Lab (sil/c/sstep_plant.c):
 * averaged shared-leg power stage, two-phase hybrid stepper and current sensing.
 * Called per PWM period, in the context of the fast loop:
 *
 *   sim_plant_sample()       ADC counts of the state at the sampling instant
 *   fast-loop callback       -> power_stage_write() -> sim_plant_set_compare()
 *   sim_plant_step()         integrate one PWM period with the voltage of the
 *                            previous compare values (one-period delay, as on
 *                            the hardware), then take the new ones
 */

#ifndef DOPPELSTEP_SIM_PLANT_H_
#define DOPPELSTEP_SIM_PLANT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doppelstep/drivers/power_stage.h>
#include <doppelstep/drivers/power_stage_sim.h>

/** Set up the model with the given scaling; rotor at rest at angle 0. */
int sim_plant_init(const struct power_stage_info *info, uint32_t microsteps,
		   uint32_t full_steps_per_rev);

/** ADC sample of the present state and the largest leg current |iA|, |iB|, |iA + iB| [mA]. ISR. */
void sim_plant_sample(struct power_stage_sample *sample, uint32_t *i_leg_max_ma);

/** Compare values for the next period. ISR. */
void sim_plant_set_compare(const uint16_t *compare, size_t legs);

/** Outputs on (windings driven) or off (windings open). Any context. */
void sim_plant_set_enabled(bool enable);

/** Advance the model by one PWM period. ISR, after the fast loop. */
void sim_plant_step(void);

/** Load torque [uN*m], see power_stage_sim_set_load(). Any context. */
void sim_plant_set_load(int32_t load_unm);

/** Bus voltage of the model [mV]. Any context. */
void sim_plant_set_vbus(uint32_t vbus_mv);

/** Shift the reported rotor position. Any context. */
void sim_plant_shift_position(int32_t delta_usteps);

/** Copy of the latest published state. Thread context. */
void sim_plant_get_state(struct sim_plant_state *state);

#endif /* DOPPELSTEP_SIM_PLANT_H_ */
