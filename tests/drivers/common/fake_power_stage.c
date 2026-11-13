/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT sfs_fake_power_stage

#include "fake_power_stage.h"

#include <errno.h>

#include <zephyr/kernel.h>

struct fake_data {
	struct power_stage_info info;
	power_stage_fast_cb_t cb;
	void *ctx;
	uint16_t compare[POWER_STAGE_LEGS_MAX];
	bool output_on;
	int32_t shift_total;
	uint32_t aux_trip_ma;
	fake_plant_fn plant;
	void *plant_ctx;
	struct k_timer plant_timer;
};

/* 20 PWM periods per millisecond, from the plant, as the sim backend does */
static void plant_tick(struct k_timer *timer)
{
	const struct device *dev = k_timer_user_data_get(timer);
	struct fake_data *data = dev->data;

	for (int k = 0; (k < 20) && (data->plant != NULL); k++) {
		struct power_stage_sample s;

		data->plant(data->compare, data->output_on, &s, data->plant_ctx);
		if (data->cb != NULL) {
			data->cb(&s, 0U, data->ctx);
		}
	}
}

void fake_power_stage_set_plant(const struct device *dev, fake_plant_fn fn, void *ctx)
{
	struct fake_data *data = dev->data;

	k_timer_stop(&data->plant_timer);
	data->plant = NULL;
	data->plant_ctx = ctx;
	data->plant = fn;
	if (fn != NULL) {
		k_timer_user_data_set(&data->plant_timer, (void *)dev);
		k_timer_start(&data->plant_timer, K_MSEC(1), K_MSEC(1));
	}
}

static int fake_init(const struct device *dev)
{
	struct fake_data *data = dev->data;

	data->info = (struct power_stage_info){
		.topology = POWER_STAGE_SHARED_LEG,
		.legs = 3,
		.outputs_allowed = true,
		.timer_clock_hz = 170000000U,
		.pwm_period_ticks = 4250,
		.pwm_freq_hz = 20000U,
		.i_full_scale_ma = 60155U,
		.adc_i_span = -2048,
		.adc_i_zero = 2048,
		.adc_vbus_mv_full = 34283U,
		.adc_full = 4095,
	};
	k_timer_init(&data->plant_timer, plant_tick, NULL);
	return 0;
}

static int fake_get_info(const struct device *dev, struct power_stage_info *info)
{
	const struct fake_data *data = dev->data;

	*info = data->info;
	return 0;
}

static int fake_calibrate(const struct device *dev, uint32_t samples)
{
	ARG_UNUSED(dev);
	return (samples == 0U) ? -EINVAL : 0;
}

static int fake_set_fast_callback(const struct device *dev, power_stage_fast_cb_t cb, void *ctx)
{
	struct fake_data *data = dev->data;

	data->cb = cb;
	data->ctx = ctx;
	return 0;
}

static int fake_write(const struct device *dev, const uint16_t *compare, size_t legs)
{
	struct fake_data *data = dev->data;

	for (size_t k = 0; k < MIN(legs, ARRAY_SIZE(data->compare)); k++) {
		data->compare[k] = compare[k];
	}
	return 0;
}

static int fake_output_enable(const struct device *dev, bool enable)
{
	struct fake_data *data = dev->data;

	data->output_on = enable;
	return 0;
}

static int fake_set_aux_trip(const struct device *dev, uint32_t trip_ma)
{
	struct fake_data *data = dev->data;

	data->aux_trip_ma = trip_ma;
	return 0;
}

static int fake_position_shifted(const struct device *dev, int32_t delta_usteps)
{
	struct fake_data *data = dev->data;

	data->shift_total += delta_usteps;
	return 0;
}

static DEVICE_API(power_stage, fake_api) = {
	.get_info = fake_get_info,
	.calibrate = fake_calibrate,
	.set_fast_callback = fake_set_fast_callback,
	.write = fake_write,
	.output_enable = fake_output_enable,
	.set_aux_trip = fake_set_aux_trip,
	.position_shifted = fake_position_shifted,
};

/* The optional operations are absent: the wrappers must answer -ENOSYS */
static DEVICE_API(power_stage, fake_minimal_api) = {
	.get_info = fake_get_info,
	.calibrate = fake_calibrate,
	.set_fast_callback = fake_set_fast_callback,
	.write = fake_write,
	.output_enable = fake_output_enable,
};

void fake_power_stage_fire(const struct device *dev, const struct power_stage_sample *sample,
			   uint32_t trips)
{
	struct fake_data *data = dev->data;

	if (data->cb != NULL) {
		data->cb(sample, trips, data->ctx);
	}
}

const uint16_t *fake_power_stage_compare(const struct device *dev)
{
	const struct fake_data *data = dev->data;

	return data->compare;
}

bool fake_power_stage_output_on(const struct device *dev)
{
	const struct fake_data *data = dev->data;

	return data->output_on;
}

void fake_power_stage_set_outputs_allowed(const struct device *dev, bool allowed)
{
	struct fake_data *data = dev->data;

	data->info.outputs_allowed = allowed;
}

int32_t fake_power_stage_shift_total(const struct device *dev)
{
	const struct fake_data *data = dev->data;

	return data->shift_total;
}

uint32_t fake_power_stage_aux_trip_ma(const struct device *dev)
{
	const struct fake_data *data = dev->data;

	return data->aux_trip_ma;
}

#define FAKE_DEFINE(n)                                                                             \
	static struct fake_data fake_data_##n;                                                     \
	DEVICE_DT_INST_DEFINE(n, fake_init, NULL, &fake_data_##n, NULL, POST_KERNEL,               \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                                  \
			      COND_CODE_1(DT_INST_PROP(n, minimal), (&fake_minimal_api),           \
					  (&fake_api)));

DT_INST_FOREACH_STATUS_OKAY(FAKE_DEFINE)
