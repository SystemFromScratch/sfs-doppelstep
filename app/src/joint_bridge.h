/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The registers of the emulated servo (x430_servo.h) mapped onto the motor: unit
 * conversions (joint.h), commands to the control task (app_msg.h), live registers
 * from its status. The only place that knows both sides. Communication task only.
 */

#ifndef DOPPELSTEP_JOINT_BRIDGE_H_
#define DOPPELSTEP_JOINT_BRIDGE_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fill the live registers and start the servo. The control task must be running. */
int joint_bridge_init(const struct device *servo);

/** Live registers from the latest status of the motor. */
void joint_bridge_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_JOINT_BRIDGE_H_ */
