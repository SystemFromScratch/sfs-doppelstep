/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Supervisor (app/src/supervisor.c): the watchdog is fed only when every task has
 * checked in since the previous check. native_sim has no watchdog: the tests check
 * the decision that supervisor_poll() returns.
 */

#include <zephyr/ztest.h>

#include "supervisor.h"

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)supervisor_poll(); /* start every test with a fresh round */
}

ZTEST(supervisor, test_without_watchdog_start_succeeds)
{
	zassert_ok(supervisor_start());
}

ZTEST(supervisor, test_no_checkin_is_not_alive)
{
	zassert_false(supervisor_poll());
}

ZTEST(supervisor, test_every_task_checked_in_is_alive)
{
	supervisor_checkin(SUPERVISOR_CONTROL);
	supervisor_checkin(SUPERVISOR_COMM);
	zassert_true(supervisor_poll());
}

ZTEST(supervisor, test_one_silent_task_is_not_alive)
{
	supervisor_checkin(SUPERVISOR_CONTROL);
	supervisor_checkin(SUPERVISOR_CONTROL);
	zassert_false(supervisor_poll(), "the comm task did not run");

	supervisor_checkin(SUPERVISOR_COMM);
	zassert_false(supervisor_poll(), "the control task did not run");
}

ZTEST(supervisor, test_a_check_starts_a_new_round)
{
	supervisor_checkin(SUPERVISOR_CONTROL);
	supervisor_checkin(SUPERVISOR_COMM);
	zassert_true(supervisor_poll());
	zassert_false(supervisor_poll(), "check-ins of the previous round do not count again");
}

ZTEST_SUITE(supervisor, NULL, NULL, before_each, NULL, NULL);
