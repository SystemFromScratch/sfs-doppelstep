/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The queues between the communication and the control task: a test thread plays
 * the control side. CONFIG_SFS_CMD_TIMEOUT_MS is 50 by default.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "app_msg.h"

#define RESPONDER_STACK 1024

K_THREAD_STACK_DEFINE(responder_stack, RESPONDER_STACK);
static struct k_thread responder_thread;

static const int *answers;
static size_t answer_count;

/* Control side: answer the next commands with the given results, in order */
static void responder_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (size_t k = 0; k < answer_count; k++) {
		struct app_cmd cmd;

		if (app_cmd_next(&cmd, K_MSEC(200)) != 0) {
			return;
		}
		app_cmd_done(&cmd, answers[k]);
	}
}

static void responder_start(const int *results, size_t count)
{
	answers = results;
	answer_count = count;
	(void)k_thread_create(&responder_thread, responder_stack, RESPONDER_STACK, responder_fn,
			      NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	k_msgq_purge(&cmd_q);
	k_msgq_purge(&rsp_q);
	k_msgq_purge(&status_q);
}

static void after_each(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)k_thread_join(&responder_thread, K_MSEC(500));
}

ZTEST(msg, test_call_returns_the_result_of_the_control_side)
{
	static const int results[] = {-EBUSY};

	responder_start(results, ARRAY_SIZE(results));
	zassert_equal(app_call(APP_CMD_MOVE_TO, 100, 200U, 300U), -EBUSY);
}

ZTEST(msg, test_command_carries_its_arguments)
{
	struct app_cmd cmd;

	/* the call times out, the command stays in the queue */
	zassert_equal(app_call(APP_CMD_MOVE_TO, -100, 200U, 300U), -ETIMEDOUT);
	zassert_ok(app_cmd_next(&cmd, K_NO_WAIT));
	zassert_equal(cmd.op, APP_CMD_MOVE_TO);
	zassert_equal(cmd.arg0, -100);
	zassert_equal(cmd.arg1, 200U);
	zassert_equal(cmd.arg2, 300U);
}

ZTEST(msg, test_call_times_out)
{
	const int64_t start = k_uptime_get();

	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), -ETIMEDOUT);
	zassert_within(k_uptime_get() - start, CONFIG_SFS_CMD_TIMEOUT_MS, 5);
}

ZTEST(msg, test_late_answer_is_discarded)
{
	static const int results[] = {-EIO, 0};

	/* The first call gives up; its answer comes before the answer of the next call */
	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), -ETIMEDOUT);
	responder_start(results, ARRAY_SIZE(results));
	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), 0);
}

ZTEST(msg, test_full_command_queue_fails_at_once)
{
	int64_t start;

	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), -ETIMEDOUT);
	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), -ETIMEDOUT);
	start = k_uptime_get();
	zassert_equal(app_call(APP_CMD_STOP, 0, 0U, 0U), -EBUSY);
	zassert_true((k_uptime_get() - start) < 5);
}

ZTEST(msg, test_done_never_blocks_and_counts_drops)
{
	const struct app_cmd cmd = {.seq = 1U};
	const uint32_t before = app_rsp_dropped();
	const int64_t start = k_uptime_get();

	app_cmd_done(&cmd, 0);
	app_cmd_done(&cmd, 0);
	app_cmd_done(&cmd, 0);
	zassert_true((k_uptime_get() - start) < 5);
	zassert_equal(app_rsp_dropped() - before, 1U);
}

ZTEST(msg, test_status_keeps_the_latest_only)
{
	struct sstep_motor_status status = {.position = 1};
	struct sstep_motor_status out;

	app_status_publish(&status);
	status.position = 2;
	app_status_publish(&status);

	zassert_ok(app_status_get(&out));
	zassert_equal(out.position, 2);
	zassert_ok(app_status_get(&out), "reading does not consume it");
	zassert_equal(out.position, 2);
}

ZTEST(msg, test_status_before_the_first_publish_is_an_error)
{
	struct sstep_motor_status out;

	zassert_equal(app_status_get(&out), -ENOMSG);
}

ZTEST_SUITE(msg, NULL, NULL, before_each, after_each, NULL);
