/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Communication task: the DYNAMIXEL bus. The servo driver parses and answers in
 * x430_servo_process() and calls joint_bridge.c, in this thread, for what the
 * registers mean. Between packets the live registers follow the motor.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include <doppelstep/drivers/x430_servo.h>

#include "app_tasks.h"
#include "joint_bridge.h"

static const struct device *const servo = DEVICE_DT_GET(DT_CHOSEN(sfs_servo));

static void comm_task(void *p1, void *p2, void *p3)
{
	int64_t next_refresh = k_uptime_get() + CONFIG_SFS_DXL_POLL_MS;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)x430_servo_process(servo, K_MSEC(CONFIG_SFS_DXL_POLL_MS));

		const int64_t now = k_uptime_get();

		if (now >= next_refresh) {
			/* After a long packet or an EEPROM write: one refresh, not a burst */
			next_refresh += CONFIG_SFS_DXL_POLL_MS;
			if (next_refresh <= now) {
				next_refresh = now + CONFIG_SFS_DXL_POLL_MS;
			}
			joint_bridge_refresh();
		}
	}
}

K_THREAD_DEFINE(comm_task_id, CONFIG_SFS_COMM_THREAD_STACK_SIZE, comm_task, NULL, NULL, NULL,
		CONFIG_SFS_COMM_THREAD_PRIORITY, 0, K_TICKS_FOREVER);
