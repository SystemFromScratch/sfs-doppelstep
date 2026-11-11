/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Supervisor: the hardware watchdog is fed only while every task runs.
 *
 * The fast loop runs in a zero-latency interrupt above every thread. A fast loop that
 * takes almost the whole PWM period, without overrunning it, leaves the threads too
 * little time: the bus stops answering and nothing can switch the motor off.
 * The overrun check of the power stage does not see that case; the watchdog does.
 * Each task checks in from its loop, and main() feeds the watchdog only when all of
 * them have checked in since the last time. A task that stops running for
 * CONFIG_SFS_WATCHDOG_TIMEOUT_MS resets the MCU, and a reset switches the outputs off.
 */

#ifndef DOPPELSTEP_SUPERVISOR_H_
#define DOPPELSTEP_SUPERVISOR_H_

#include <stdbool.h>

enum supervisor_task {
	SUPERVISOR_CONTROL, /**< control_task: slow loop and commands */
	SUPERVISOR_COMM,    /**< comm_task: DYNAMIXEL bus */
	SUPERVISOR_TASKS,
};

/** A task's loop ran. Any thread, any time; no kernel call. */
void supervisor_checkin(enum supervisor_task task);

/**
 * Logs whether the last reset came from the watchdog and starts it
 * (CONFIG_SFS_WATCHDOG). Call once, after the tasks are running.
 * @retval 0 started, or the watchdog is not part of the build.
 */
int supervisor_start(void);

/**
 * Every CONFIG_SFS_WATCHDOG_CHECK_MS, from main(). Feeds the watchdog if every task has
 * checked in since the previous call, and starts the next round.
 * @return true when every task had checked in.
 */
bool supervisor_poll(void);

#endif /* DOPPELSTEP_SUPERVISOR_H_ */
