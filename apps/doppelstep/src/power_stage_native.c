/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power stage of native_sim (see power_stage.h): no hardware, the simulated
 * motor (sim_plant.c) is always behind it. A kernel timer expiring every tick
 * takes the place of the ADC interrupt of power_stage.c and runs, for each
 * PWM period of the tick, the same sequence:
 *
 *   sim_plant_sample() -> fast-loop callback (Stepper-C) -> sim_plant_step()
 *
 * Code takes no simulated time, so a batch of PWM periods per tick changes
 * nothing for Stepper-C (the slow-loop thread wakes after the timer, every
 * PWM_FREQ_HZ / slow_loop_hz periods as on the MCU), while the host wakes up
 * only once per tick instead of 20000 times per second.
 *
 * The scaling is the one power_stage.c derives from the power-stage node of
 * boards/b_g431b_esc1.overlay, so Stepper-C runs with the ESC1 numbers.
 *
 * Command line options of zephyr.exe set the scenario of the model without a
 * rebuild (tests, tests/README.md):
 *   -sim_load_unm=<uN*m>  load torque (CONFIG_SFS_SIM_LOAD_UNM otherwise)
 *   -sim_vbus_mv=<mV>     bus voltage (CONFIG_SFS_SIM_VBUS_MV otherwise)
 */

#include "power_stage.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include "sim_plant.h"

LOG_MODULE_REGISTER(power_stage, CONFIG_SFS_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_SFS_SIM_PLANT), "native_sim needs the simulated motor");

/* boards/b_g431b_esc1.overlay and the ESC1 clock tree */
#define TIMER_CLOCK_HZ 170000000U
#define PWM_FREQ_HZ 20000U
#define ADC_VREF_MV 3300U
#define SHUNT_UOHM 3000U
#define SENSE_GAIN_MILLI (-9143)
#define VBUS_R_LOW 18000U
#define VBUS_R_HIGH 169000U

#define ADC_BITS 12U
#define ADC_FULL ((1U << ADC_BITS) - 1U)
#define ADC_MID (1U << (ADC_BITS - 1U))

BUILD_ASSERT((PWM_FREQ_HZ % CONFIG_SYS_CLOCK_TICKS_PER_SEC) == 0,
	     "the PWM rate must be a multiple of the tick rate");

#define PERIODS_PER_TICK (PWM_FREQ_HZ / CONFIG_SYS_CLOCK_TICKS_PER_SEC)

static struct power_stage_info info;

static volatile power_stage_fast_fn fast_fn;
static void *volatile fast_ctx;
static volatile uint32_t w_trip_ma; /* 0 = disabled */
static volatile uint32_t isr_count;

static void fast_timer_fn(struct k_timer *timer)
{
	const power_stage_fast_fn fn = fast_fn;

	ARG_UNUSED(timer);
	for (uint32_t k = 0U; k < PERIODS_PER_TICK; k++) {
		isr_count++;
		if (fn != NULL) {
			struct sstep_sample sample;
			uint32_t i_w_ma;
			const uint32_t trip = w_trip_ma;

			sim_plant_sample(&sample, &i_w_ma);
			fn(&sample, (trip != 0U) && (i_w_ma > trip), fast_ctx);
			sim_plant_step();
		}
	}
}

static K_TIMER_DEFINE(fast_timer, fast_timer_fn, NULL);

/* Command line scenario; native_sim sets the destination of an option that
 * is not given to its type's default, hence the flags.
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

int power_stage_init(void)
{
	const uint32_t gain_abs = (uint32_t)((SENSE_GAIN_MILLI < 0) ? -SENSE_GAIN_MILLI
								     : SENSE_GAIN_MILLI);
	int ret;

	info.timer_clock_hz = TIMER_CLOCK_HZ;
	info.pwm_freq_hz = PWM_FREQ_HZ;
	info.pwm_period_ticks = (uint16_t)(TIMER_CLOCK_HZ / (2U * PWM_FREQ_HZ));
	info.dead_time_ns = 0U;
	info.i_full_scale_ma =
		(uint32_t)(((uint64_t)ADC_VREF_MV * ADC_MID / (ADC_FULL + 1U)) * 1000000000ULL /
			   ((uint64_t)SHUNT_UOHM * gain_abs));
	info.adc_i_span = (SENSE_GAIN_MILLI < 0) ? -(int32_t)ADC_MID : (int32_t)ADC_MID;
	if (IS_ENABLED(CONFIG_SFS_STAGE_CURRENT_SENSE_INVERT)) {
		info.adc_i_span = -info.adc_i_span;
	}
	info.adc_i_zero = (uint16_t)ADC_MID;
	info.adc_vbus_mv_full =
		(uint32_t)((uint64_t)ADC_VREF_MV * (VBUS_R_LOW + VBUS_R_HIGH) / VBUS_R_LOW);
	info.adc_full = (uint16_t)ADC_FULL;

	ret = sim_plant_init(&info);
	if (ret != 0) {
		LOG_ERR("simulated motor (%d)", ret);
		return ret;
	}
	if (arg_load_given) {
		sim_plant_set_load(arg_load_unm);
		LOG_INF("load %d uNm (command line)", arg_load_unm);
	}
	if (arg_vbus_given && (arg_vbus_mv >= 0)) {
		sim_plant_set_vbus((uint32_t)arg_vbus_mv);
		LOG_INF("bus voltage %d mV (command line)", arg_vbus_mv);
	}

	k_timer_start(&fast_timer, K_TICKS(1), K_TICKS(1));
	LOG_INF("native power stage: %u fast-loop periods (%u Hz) per tick, "
		"current full scale %u mA",
		PERIODS_PER_TICK, PWM_FREQ_HZ, info.i_full_scale_ma);
	return 0;
}

const struct power_stage_info *power_stage_get_info(void)
{
	return &info;
}

int power_stage_calibrate(uint32_t samples)
{
	/* the model has no sensor offset */
	return (samples == 0U) ? -EINVAL : 0;
}

void power_stage_set_fast_fn(power_stage_fast_fn fn, void *ctx)
{
	fast_fn = NULL;
	fast_ctx = ctx;
	fast_fn = fn;
}

void power_stage_set_w_trip(uint32_t trip_ma)
{
	w_trip_ma = trip_ma;
}

int power_stage_pwm_write(void *context, const uint16_t compare[SSTEP_LEGS_MAX])
{
	ARG_UNUSED(context);
	sim_plant_set_compare(compare);
	return 0;
}

int power_stage_output_enable(void *context, bool enable)
{
	ARG_UNUSED(context);
	sim_plant_set_enabled(enable);
	return 0;
}

uint32_t power_stage_isr_count(void)
{
	return isr_count;
}

uint32_t power_stage_fast_cycles_max(bool reset)
{
	/* simulated time: code takes none */
	ARG_UNUSED(reset);
	return 0U;
}
