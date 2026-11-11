/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulation backend of the power_stage class: the motor model of sim_plant.c
 * takes the place of the hardware. Per PWM period:
 *
 *   sim_plant_sample() -> fast-loop callback -> sim_plant_step()
 *
 * Two sources of the period, chosen by the devicetree:
 *
 * - Without the timebase property (native_sim): a kernel timer expiring every
 *   tick runs the PWM periods of the tick in one batch. Code takes no simulated
 *   time, so a batch changes nothing for the controller, while the host wakes up
 *   only once per tick instead of 20000 times per second. Scaling: properties of
 *   the node.
 *
 * - With timebase = <&power_stage> (on the board): the fast-loop interrupt of the
 *   real stage keeps its timing and its scaling, and its samples are ignored. This
 *   file never switches the outputs of that stage, so the gate drivers stay off
 *   whatever the dead time is. scripts/check-layers.sh checks that.
 *
 * Command line options of zephyr.exe set the scenario without a rebuild:
 *   -sim_load_unm=<uN*m>  load torque (CONFIG_SFS_SIM_LOAD_UNM otherwise)
 *   -sim_vbus_mv=<mV>     bus voltage (CONFIG_SFS_SIM_VBUS_MV otherwise)
 */

#define DT_DRV_COMPAT sfs_sim_power_stage

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <doppelstep/drivers/power_stage.h>
#include <doppelstep/drivers/power_stage_sim.h>

#include "power_stage_scaling.h"
#include "sim_plant.h"

LOG_MODULE_REGISTER(power_stage_sim, CONFIG_POWER_STAGE_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "the model exists once");

#define SIM_NODE     DT_DRV_INST(0)
#define HAS_TIMEBASE DT_NODE_HAS_PROP(SIM_NODE, timebase)

#define ADC_BITS 12U

static struct power_stage_info info;

static volatile power_stage_fast_cb_t fast_cb;
static void *volatile fast_ctx;
static volatile bool offsets_valid;   /* as on real hardware: no trip before calibration */
static volatile uint32_t aux_trip_ma; /* 0 = disabled */
static volatile uint32_t isr_count;

/* One PWM period. `trips`: POWER_STAGE_TRIP_OVERRUN of the timebase, if any. */
static void model_period(uint32_t trips)
{
	const power_stage_fast_cb_t cb = fast_cb;
	struct power_stage_sample sample;
	uint32_t i_w_ma;
	const uint32_t trip = aux_trip_ma;

	isr_count++;
	sim_plant_sample(&sample, &i_w_ma);
	if ((trip != 0U) && offsets_valid && (i_w_ma > trip)) {
		trips |= POWER_STAGE_TRIP_AUX;
	}
	if (cb != NULL) {
		cb(&sample, trips, fast_ctx);
	}
	sim_plant_step();
}

#if HAS_TIMEBASE

static const struct device *const timebase = DEVICE_DT_GET(DT_PHANDLE(SIM_NODE, timebase));

/* The overrun check of the real stage covers the model and the controller together:
 * forwarded, so that the controller faults instead of the board hanging.
 */
static void timebase_cb(const struct power_stage_sample *sample, uint32_t trips, void *ctx)
{
	ARG_UNUSED(sample);
	ARG_UNUSED(ctx);
	model_period(trips & POWER_STAGE_TRIP_OVERRUN);
}

static int period_source_start(void)
{
	return power_stage_set_fast_callback(timebase, timebase_cb, NULL);
}

static int scaling_init(void)
{
	int ret;

	if (!device_is_ready(timebase)) {
		return -ENODEV;
	}
	ret = power_stage_get_info(timebase, &info);
	if (ret == 0) {
		/* the model has no gate drivers to protect */
		info.outputs_allowed = true;
	}
	return ret;
}

#else /* HAS_TIMEBASE */

#define PWM_FREQ_HZ DT_PROP(SIM_NODE, pwm_frequency)

BUILD_ASSERT((PWM_FREQ_HZ % CONFIG_SYS_CLOCK_TICKS_PER_SEC) == 0,
	     "the PWM rate must be a multiple of the tick rate");

#define PERIODS_PER_TICK (PWM_FREQ_HZ / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

static void fast_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	for (uint32_t k = 0U; k < PERIODS_PER_TICK; k++) {
		model_period(0U);
	}
}

static K_TIMER_DEFINE(fast_timer, fast_timer_fn, NULL);

static int period_source_start(void)
{
	k_timer_start(&fast_timer, K_TICKS(1), K_TICKS(1));
	LOG_INF("native power stage: %u fast-loop periods (%u Hz) per tick, "
		"current full scale %u mA",
		PERIODS_PER_TICK, PWM_FREQ_HZ, info.i_full_scale_ma);
	return 0;
}

static int scaling_init(void)
{
	const uint32_t timer_clock_hz = DT_PROP(SIM_NODE, timer_clock_hz);

	info.topology = POWER_STAGE_SHARED_LEG;
	info.legs = 3U;
	info.outputs_allowed = true;
	info.timer_clock_hz = timer_clock_hz;
	info.pwm_freq_hz = PWM_FREQ_HZ;
	info.pwm_period_ticks = (uint16_t)(timer_clock_hz / (2U * PWM_FREQ_HZ));
	info.dead_time_ns = 0U;
	power_stage_scale(&info, ADC_BITS, DT_PROP(SIM_NODE, adc_vref_mv),
			  DT_PROP(SIM_NODE, shunt_micro_ohms),
			  DT_PROP(SIM_NODE, current_sense_gain_milli),
			  DT_PROP_BY_IDX(SIM_NODE, vbus_divider, 0),
			  DT_PROP_BY_IDX(SIM_NODE, vbus_divider, 1),
			  DT_PROP(SIM_NODE, current_sense_invert));
	return 0;
}

#endif /* HAS_TIMEBASE */

/* ------------------------------------------------------------------------ */
/* Scenario from the command line                                            */

#if defined(CONFIG_ARCH_POSIX)

#include <cmdline.h>
#include <posix_native_task.h>

/* native_sim sets the destination of an option that is not given to its type's
 * default, hence the flags.
 */
static int32_t arg_load_unm;
static int32_t arg_vbus_mv;
static bool arg_load_given;
static bool arg_vbus_given;

static void load_given(char *argv, int offset)
{
	ARG_UNUSED(argv);
	ARG_UNUSED(offset);
	arg_load_given = true;
}

static void vbus_given(char *argv, int offset)
{
	ARG_UNUSED(argv);
	ARG_UNUSED(offset);
	arg_vbus_given = true;
}

static void add_sim_options(void)
{
	static struct args_struct_t sim_options[] = {
		{
			.option = "sim_load_unm",
			.name = "uN*m",
			.type = 'i',
			.dest = &arg_load_unm,
			.call_when_found = load_given,
			.descript = "Load torque of the simulated motor (default "
				    "CONFIG_SFS_SIM_LOAD_UNM)",
		},
		{
			.option = "sim_vbus_mv",
			.name = "mV",
			.type = 'i',
			.dest = &arg_vbus_mv,
			.call_when_found = vbus_given,
			.descript = "Bus voltage of the simulated motor (default "
				    "CONFIG_SFS_SIM_VBUS_MV)",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(sim_options);
}

NATIVE_TASK(add_sim_options, PRE_BOOT_1, 10);

static void scenario_apply(void)
{
	if (arg_load_given) {
		sim_plant_set_load(arg_load_unm);
		LOG_INF("load %d uNm (command line)", arg_load_unm);
	}
	if (arg_vbus_given && (arg_vbus_mv >= 0)) {
		sim_plant_set_vbus((uint32_t)arg_vbus_mv);
		LOG_INF("bus voltage %d mV (command line)", arg_vbus_mv);
	}
}

#else

static void scenario_apply(void)
{
}

#endif /* CONFIG_ARCH_POSIX */

/* ------------------------------------------------------------------------ */
/* power_stage API                                                           */

static int sim_get_info(const struct device *dev, struct power_stage_info *out)
{
	ARG_UNUSED(dev);
	*out = info;
	return 0;
}

static int sim_calibrate(const struct device *dev, uint32_t samples)
{
	ARG_UNUSED(dev);
	if (samples == 0U) {
		return -EINVAL;
	}
	/* the model has no sensor offset */
	offsets_valid = true;
	return 0;
}

static int sim_set_fast_callback(const struct device *dev, power_stage_fast_cb_t cb, void *ctx)
{
	ARG_UNUSED(dev);
	fast_cb = NULL;
	fast_ctx = ctx;
	fast_cb = cb;
	return 0;
}

static int sim_write(const struct device *dev, const uint16_t *compare, size_t legs)
{
	ARG_UNUSED(dev);
	sim_plant_set_compare(compare, legs);
	return 0;
}

static int sim_output_enable(const struct device *dev, bool enable)
{
	ARG_UNUSED(dev);
	sim_plant_set_enabled(enable);
	return 0;
}

static int sim_set_aux_trip(const struct device *dev, uint32_t trip_ma)
{
	ARG_UNUSED(dev);
	aux_trip_ma = trip_ma;
	return 0;
}

static int sim_get_stats(const struct device *dev, struct power_stage_stats *stats)
{
	ARG_UNUSED(dev);
#if HAS_TIMEBASE
	int ret = power_stage_get_stats(timebase, stats);

	if (ret != 0) {
		return ret;
	}
#else
	/* simulated time: code takes none */
	stats->fast_cycles_max = 0U;
	stats->overruns = 0U;
#endif
	stats->isr_count = isr_count;
	return 0;
}

static int sim_position_shifted(const struct device *dev, int32_t delta_usteps)
{
	ARG_UNUSED(dev);
	sim_plant_shift_position(delta_usteps);
	return 0;
}

int power_stage_sim_set_load(const struct device *dev, int32_t load_unm)
{
	ARG_UNUSED(dev);
	sim_plant_set_load(load_unm);
	return 0;
}

int power_stage_sim_set_vbus(const struct device *dev, uint32_t vbus_mv)
{
	ARG_UNUSED(dev);
	sim_plant_set_vbus(vbus_mv);
	return 0;
}

int power_stage_sim_get_state(const struct device *dev, struct sim_plant_state *state)
{
	ARG_UNUSED(dev);
	sim_plant_get_state(state);
	return 0;
}

static int sim_init(const struct device *dev)
{
	int ret;

	ARG_UNUSED(dev);

	ret = scaling_init();
	if (ret != 0) {
		return ret;
	}
	ret = sim_plant_init(&info, DT_PROP(SIM_NODE, microsteps),
			     DT_PROP(SIM_NODE, full_steps_per_rev));
	if (ret != 0) {
		LOG_ERR("simulated motor (%d)", ret);
		return ret;
	}
	scenario_apply();
	return period_source_start();
}

static DEVICE_API(power_stage, sim_api) = {
	.get_info = sim_get_info,
	.calibrate = sim_calibrate,
	.set_fast_callback = sim_set_fast_callback,
	.write = sim_write,
	.output_enable = sim_output_enable,
	.set_aux_trip = sim_set_aux_trip,
	.get_stats = sim_get_stats,
	.position_shifted = sim_position_shifted,
};

DEVICE_DT_INST_DEFINE(0, sim_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_POWER_STAGE_SIM_INIT_PRIORITY, &sim_api);
