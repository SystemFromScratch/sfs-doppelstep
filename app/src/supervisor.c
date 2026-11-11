/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Supervisor: hardware watchdog fed only while every task runs (supervisor.h).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "supervisor.h"

#if defined(CONFIG_SFS_WATCHDOG)
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#endif

LOG_MODULE_REGISTER(supervisor, CONFIG_SFS_LOG_LEVEL);

#define ALL_TASKS (BIT(SUPERVISOR_TASKS) - 1U)

static atomic_t alive;

#if defined(CONFIG_SFS_WATCHDOG)
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;
#endif

void supervisor_checkin(enum supervisor_task task)
{
	(void)atomic_or(&alive, (atomic_val_t)BIT(task));
}

int supervisor_start(void)
{
#if defined(CONFIG_SFS_WATCHDOG)
	const struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = CONFIG_SFS_WATCHDOG_TIMEOUT_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	uint32_t cause = 0U;
	int ret;

	if ((hwinfo_get_reset_cause(&cause) == 0) && ((cause & RESET_WATCHDOG) != 0U)) {
		LOG_WRN("reset by the watchdog: a task did not run for %u ms",
			CONFIG_SFS_WATCHDOG_TIMEOUT_MS);
	}
	(void)hwinfo_clear_reset_cause();

	if (!device_is_ready(wdt)) {
		return -ENODEV;
	}
	ret = wdt_install_timeout(wdt, &cfg);
	if (ret < 0) {
		return ret;
	}
	wdt_channel = ret;
	/* A debugger that halts the CPU halts the watchdog too */
	ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret != 0) {
		return ret;
	}
	LOG_INF("watchdog %u ms", CONFIG_SFS_WATCHDOG_TIMEOUT_MS);
#endif
	return 0;
}

bool supervisor_poll(void)
{
	const bool all = ((uint32_t)atomic_clear(&alive) & ALL_TASKS) == ALL_TASKS;

#if defined(CONFIG_SFS_WATCHDOG)
	if (all && (wdt_channel >= 0)) {
		(void)wdt_feed(wdt, wdt_channel);
	}
#endif
	return all;
}
