/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Periodic log line of the simulated motor: the commanded position next to the
 * rotor of the model. It needs both sides, so it lives in the application. The
 * tests parse it (tests/firmware.py): keep the format and the module name.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <doppelstep/drivers/power_stage.h>
#include <doppelstep/drivers/power_stage_sim.h>

#include "app_msg.h"

LOG_MODULE_DECLARE(sim_plant, CONFIG_POWER_STAGE_LOG_LEVEL);

#if CONFIG_SFS_SIM_LOG_PERIOD_MS > 0

#define MOTOR_NODE DT_CHOSEN(sfs_motor)
#define STAGE_NODE DT_PHANDLE(MOTOR_NODE, power_stage)
#define MICROSTEPS DT_PROP(MOTOR_NODE, microsteps)

BUILD_ASSERT(DT_PROP(STAGE_NODE, microsteps) == MICROSTEPS,
	     "microsteps of the simulated stage and of the controller differ");

static const struct device *const stage = DEVICE_DT_GET(STAGE_NODE);

static void log_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(log_work, log_fn);

static void log_fn(struct k_work *work)
{
	struct sim_plant_state s;
	struct sstep_motor_status status = {0};
	struct power_stage_stats stats;
	struct power_stage_info info;

	ARG_UNUSED(work);
	(void)power_stage_sim_get_state(stage, &s);
	(void)app_status_get(&status);

	/* rotor behind the command, in electrical degrees: above 90 the motor is
	 * close to losing steps, a multiple of 360 means 4 full steps lost
	 */
	const int32_t lag = status.position - s.rotor_usteps;

	LOG_INF("cmd %d rotor %d usteps (lag %d deg el), %d rpm, iA %d iB %d mA, "
		"vA %d vB %d mV, load %d uNm",
		status.position, s.rotor_usteps, lag * 90 / MICROSTEPS,
		s.speed_mrpm / 1000, s.i_a_ma, s.i_b_ma, s.v_a_mv, s.v_b_mv, s.load_unm);

	/* native_sim: code takes no simulated time, the figure stays 0 */
	if ((power_stage_get_stats(stage, &stats) == 0) && (stats.fast_cycles_max != 0U) &&
	    (power_stage_get_info(stage, &info) == 0)) {
		LOG_INF("fast loop max %u cycles (%u us of %u)", stats.fast_cycles_max,
			(uint32_t)((uint64_t)stats.fast_cycles_max * 1000000U /
				   sys_clock_hw_cycles_per_sec()),
			1000000U / info.pwm_freq_hz);
	}

	(void)k_work_reschedule(&log_work, K_MSEC(CONFIG_SFS_SIM_LOG_PERIOD_MS));
}

static int sim_log_start(void)
{
	(void)k_work_schedule(&log_work, K_MSEC(CONFIG_SFS_SIM_LOG_PERIOD_MS));
	return 0;
}

SYS_INIT(sim_log_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_SFS_SIM_LOG_PERIOD_MS > 0 */
