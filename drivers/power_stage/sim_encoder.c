/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated shaft encoder (compatible "sfs,sim-encoder"): the rotor angle of the motor
 * model behind a sfs,sim-power-stage, through the sensor API, so that the encoder path
 * of the application runs in the SIL tests.
 *
 * On native_sim the encoder is fitted only with the command line options
 *   -sim_encoder                      encoder on the shaft
 *   -sim_encoder_mount=<counts>       its reading at the start angle (mount-offset otherwise)
 * Without -sim_encoder the device is not ready, and the application runs open loop as on
 * a board without an encoder (the B-G431B-ESC1).
 */

#define DT_DRV_COMPAT sfs_sim_encoder

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <doppelstep/drivers/power_stage_sim.h>

struct sim_encoder_config {
	const struct device *stage;
	uint32_t usteps_per_rev;
	uint16_t mount_offset;
};

struct sim_encoder_data {
	uint16_t angle; /* [1/65536 revolution] */
	uint16_t mount; /* reading at the start angle */
};

static int sim_encoder_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct sim_encoder_config *cfg = dev->config;
	struct sim_encoder_data *data = dev->data;
	struct sim_plant_state s;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_ROTATION)) {
		return -ENOTSUP;
	}
	ret = power_stage_sim_get_state(cfg->stage, &s);
	if (ret != 0) {
		return ret;
	}
	const int64_t turn = (int64_t)cfg->usteps_per_rev;
	const int64_t usteps = (((int64_t)s.rotor_abs_usteps % turn) + turn) % turn;

	data->angle = (uint16_t)((usteps * 65536) / turn + data->mount);
	return 0;
}

static int sim_encoder_get(const struct device *dev, enum sensor_channel chan,
			   struct sensor_value *val)
{
	const struct sim_encoder_data *data = dev->data;
	const uint64_t udeg = ((uint64_t)data->angle * 360000000ULL) / 65536U;

	if (chan != SENSOR_CHAN_ROTATION) {
		return -ENOTSUP;
	}
	val->val1 = (int32_t)(udeg / 1000000U);
	val->val2 = (int32_t)(udeg % 1000000U);
	return 0;
}

static DEVICE_API(sensor, sim_encoder_api) = {
	.sample_fetch = sim_encoder_fetch,
	.channel_get = sim_encoder_get,
};

#if defined(CONFIG_ARCH_POSIX)
#include <cmdline.h>
#include <posix_native_task.h>

static bool arg_fitted;
static int32_t arg_mount;
static bool arg_mount_given;

static void mount_given(char *argv, int offset)
{
	ARG_UNUSED(argv);
	ARG_UNUSED(offset);
	arg_mount_given = true;
}

static void add_encoder_options(void)
{
	static struct args_struct_t options[] = {
		{
			.is_switch = true,
			.option = "sim_encoder",
			.type = 'b',
			.dest = &arg_fitted,
			.descript = "Fit the simulated shaft encoder (sfs,sim-encoder)",
		},
		{
			.option = "sim_encoder_mount",
			.name = "counts",
			.type = 'i',
			.dest = &arg_mount,
			.call_when_found = mount_given,
			.descript = "Encoder reading at the start angle of the rotor [1/65536 rev]",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_encoder_options, PRE_BOOT_1, 11);
#endif

static int sim_encoder_init(const struct device *dev)
{
	const struct sim_encoder_config *cfg = dev->config;

#if defined(CONFIG_ARCH_POSIX)
	struct sim_encoder_data *data = dev->data;

	if (!arg_fitted) {
		return -ENODEV; /* no encoder: the application runs open loop */
	}
	data->mount = arg_mount_given ? (uint16_t)arg_mount : cfg->mount_offset;
#else
	struct sim_encoder_data *data = dev->data;

	data->mount = cfg->mount_offset;
#endif
	return device_is_ready(cfg->stage) ? 0 : -ENODEV;
}

#define SIM_ENCODER(n)                                                                             \
	static struct sim_encoder_data sim_encoder_data_##n;                                       \
	static const struct sim_encoder_config sim_encoder_config_##n = {                          \
		.stage = DEVICE_DT_GET(DT_INST_PHANDLE(n, stage)),                                 \
		.usteps_per_rev = DT_PROP(DT_INST_PHANDLE(n, stage), microsteps) *                 \
				  DT_PROP(DT_INST_PHANDLE(n, stage), full_steps_per_rev),          \
		.mount_offset = DT_INST_PROP(n, mount_offset),                                     \
	};                                                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(n, sim_encoder_init, NULL, &sim_encoder_data_##n,             \
				     &sim_encoder_config_##n, POST_KERNEL, 85, &sim_encoder_api);

DT_INST_FOREACH_STATUS_OKAY(SIM_ENCODER)
