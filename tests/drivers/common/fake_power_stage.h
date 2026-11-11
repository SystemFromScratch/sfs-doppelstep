/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake power stage for driver tests: the test fires the fast loop itself and
 * inspects what the driver under test did with the outputs. Scaling: ESC1.
 */

#ifndef FAKE_POWER_STAGE_H
#define FAKE_POWER_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#include <doppelstep/drivers/power_stage.h>

void fake_power_stage_fire(const struct device *dev, const struct power_stage_sample *sample,
			   uint32_t trips);
const uint16_t *fake_power_stage_compare(const struct device *dev);
bool fake_power_stage_output_on(const struct device *dev);
void fake_power_stage_set_outputs_allowed(const struct device *dev, bool allowed);
int32_t fake_power_stage_shift_total(const struct device *dev);
uint32_t fake_power_stage_aux_trip_ma(const struct device *dev);

#endif /* FAKE_POWER_STAGE_H */
