/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MA730 driver against its emulator: angle frames, register reads, field flags, the
 * sensor API, reverse and zero offset, no write to the non-volatile memory, and the two
 * frames of a register read kept together against another thread.
 */

#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/ztest.h>

#include <doppelstep/drivers/ma730.h>

#include "ma730_emul.h"

static const struct device *const enc = DEVICE_DT_GET(DT_NODELABEL(encoder));
static const struct device *const enc_rev = DEVICE_DT_GET(DT_NODELABEL(encoder_rev));
static const struct emul *const emul = EMUL_DT_GET(DT_NODELABEL(encoder));
static const struct emul *const emul_rev = EMUL_DT_GET(DT_NODELABEL(encoder_rev));

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	ma730_emul_set_angle(emul, 0U);
	ma730_emul_set_angle(emul_rev, 0U);
	ma730_emul_set_reg(emul, MA730_REG_FIELD, 0U);
	ma730_emul_set_frame_hook(emul, NULL, NULL);
}

ZTEST(ma730, test_devices_are_ready)
{
	zassert_true(device_is_ready(enc));
	zassert_true(device_is_ready(enc_rev));
}

ZTEST(ma730, test_angle_is_one_frame)
{
	const uint32_t frames = ma730_emul_frames(emul);
	uint16_t angle;

	ma730_emul_set_angle(emul, 0x4000U); /* a quarter turn */
	zassert_ok(ma730_read_angle(enc, &angle));
	zassert_equal(angle, 0x4000U);
	zassert_equal(ma730_emul_frames(emul), frames + 1U);
}

ZTEST(ma730, test_register_read_takes_two_frames)
{
	const uint32_t frames = ma730_emul_frames(emul);
	uint8_t value = 0U;

	ma730_emul_set_reg(emul, 0x09U, 0x80U);
	zassert_ok(ma730_read_reg(enc, 0x09U, &value));
	zassert_equal(value, 0x80U);
	zassert_equal(ma730_emul_frames(emul), frames + 2U);
	zassert_equal(ma730_read_reg(enc, 0x20U, &value), -EINVAL, "5-bit address");
}

ZTEST(ma730, test_field_flags)
{
	bool high = true;
	bool low = true;

	zassert_ok(ma730_read_field(enc, &high, &low));
	zassert_false(high);
	zassert_false(low);

	ma730_emul_set_reg(emul, MA730_REG_FIELD, MA730_FIELD_LOW);
	zassert_ok(ma730_read_field(enc, &high, &low));
	zassert_false(high);
	zassert_true(low, "magnet too far");

	ma730_emul_set_reg(emul, MA730_REG_FIELD, MA730_FIELD_HIGH);
	zassert_ok(ma730_read_field(enc, &high, &low));
	zassert_true(high, "magnet too close");
	zassert_false(low);
}

ZTEST(ma730, test_after_a_register_read_the_angle_comes_back)
{
	uint8_t value;
	uint16_t angle;

	ma730_emul_set_angle(emul, 1234U);
	zassert_ok(ma730_read_reg(enc, MA730_REG_FIELD, &value));
	zassert_ok(ma730_read_angle(enc, &angle));
	zassert_equal(angle, 1234U);
}

ZTEST(ma730, test_sensor_api_reports_degrees)
{
	struct sensor_value val;

	ma730_emul_set_angle(emul, 0x8000U); /* half a turn */
	zassert_ok(sensor_sample_fetch(enc));
	zassert_ok(sensor_channel_get(enc, SENSOR_CHAN_ROTATION, &val));
	zassert_equal(val.val1, 180);
	zassert_equal(val.val2, 0);

	ma730_emul_set_angle(emul, 1U); /* one count: 360 / 65536 degrees */
	zassert_ok(sensor_sample_fetch_chan(enc, SENSOR_CHAN_ROTATION));
	zassert_ok(sensor_channel_get(enc, SENSOR_CHAN_ROTATION, &val));
	zassert_equal(val.val1, 0);
	zassert_equal(val.val2, 5493); /* 5493.16 micro-degrees */

	zassert_equal(sensor_channel_get(enc, SENSOR_CHAN_ACCEL_X, &val), -ENOTSUP);
	zassert_equal(sensor_sample_fetch_chan(enc, SENSOR_CHAN_ACCEL_X), -ENOTSUP);
}

ZTEST(ma730, test_reverse_and_zero_offset)
{
	uint16_t angle;

	/* reversed: 0 - raw, then minus the offset of 1000 */
	ma730_emul_set_angle(emul_rev, 1000U);
	zassert_ok(ma730_read_angle(enc_rev, &angle));
	zassert_equal(angle, (uint16_t)(0U - 1000U - 1000U));

	ma730_emul_set_angle(emul_rev, (uint16_t)(0U - 1000U));
	zassert_ok(ma730_read_angle(enc_rev, &angle));
	zassert_equal(angle, 0U, "the raw reading of the zero position gives 0");
}

ZTEST(ma730, test_the_driver_never_writes_the_nvm)
{
	uint16_t angle;
	uint8_t value;

	zassert_ok(ma730_read_angle(enc, &angle));
	zassert_ok(ma730_read_reg(enc, 0x00U, &value));
	zassert_equal(ma730_emul_writes(emul), 0U);
	zassert_equal(ma730_emul_writes(emul_rev), 0U);
}

/* A register read in a thread of lower priority than the test. Right after its read
 * command frame the emulator hook wakes the test thread, which preempts it and reads the
 * angle before the second frame of the register read.
 */
#define READER_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(reader_stack, READER_STACK_SIZE);
static struct k_thread reader_thread;
static K_SEM_DEFINE(command_sent, 0, 1);
static uint8_t reader_value;
static int reader_ret;

static void wake_test_after_read_command(uint16_t tx, void *ctx)
{
	ARG_UNUSED(ctx);
	if ((tx >> 13) == 0x2U) { /* 010: read command */
		ma730_emul_set_frame_hook(emul, NULL, NULL);
		k_sem_give(&command_sent);
	}
}

static void reader(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	reader_ret = ma730_read_reg(enc, MA730_REG_FIELD, &reader_value);
}

ZTEST(ma730, test_register_read_is_not_split_by_another_thread)
{
	const int prio = k_thread_priority_get(k_current_get());
	uint16_t angle = 0U;

	ma730_emul_set_angle(emul, 0x04D2U);
	ma730_emul_set_reg(emul, MA730_REG_FIELD, MA730_FIELD_HIGH | MA730_FIELD_LOW);
	ma730_emul_set_frame_hook(emul, wake_test_after_read_command, NULL);
	k_sem_reset(&command_sent);
	reader_value = 0U;
	reader_ret = -1;

	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader, NULL, NULL, NULL, prio + 1, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&command_sent, K_SECONDS(1)), "no read command");

	/* The reader is between its two frames */
	zassert_ok(ma730_read_angle(enc, &angle));
	zassert_ok(k_thread_join(&reader_thread, K_SECONDS(1)));

	zassert_equal(angle, 0x04D2U, "the angle frame took the register answer");
	zassert_ok(reader_ret);
	zassert_equal(reader_value, MA730_FIELD_HIGH | MA730_FIELD_LOW,
		      "the register read took the angle");
}

ZTEST_SUITE(ma730, NULL, NULL, before_each, NULL, NULL);
