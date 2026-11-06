/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The application tasks. They are created stopped; main() starts them after a
 * successful initialisation.
 */

#ifndef DOPPELSTEP_APP_TASKS_H_
#define DOPPELSTEP_APP_TASKS_H_

#include <zephyr/kernel.h>

extern const k_tid_t control_task_id;
extern const k_tid_t comm_task_id;

#endif /* DOPPELSTEP_APP_TASKS_H_ */
