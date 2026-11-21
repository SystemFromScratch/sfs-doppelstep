/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPS MagAlpha MA730 magnetic angle sensor on SPI (MA730 datasheet rev. 1.1):
 *
 *   angle      one 16-bit frame, MOSI 0x0000, MISO = angle (15:0), MSB first
 *   read reg   frame 1: MOSI 010 A4..A0 00000000, frame 2: MOSI 0x0000, MISO = value in
 *              the upper byte; at least 750 ns idle before and after frame 2
 *
 * Writes go to non-volatile memory (1000 cycles, 20 ms each): this driver does none.
 */

#define DT_DRV_COMPAT sfs_ma730

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <doppelstep/drivers/ma730.h>

LOG_MODULE_REGISTER(ma730, CONFIG_SENSOR_LOG_LEVEL);

#define MA730_CMD_READ 0x4000U /* 010 in bits 15..13 */
#define MA730_IDLE_US  1U      /* >= 750 ns between the frames of a register read */

/* SPI mode 3 (the MA730 detects mode 0 or 3), controller, 8-bit words, MSB first */
#define MA730_SPI_OP (SPI_OP_MODE_CONTROLLER | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_WORD_SET(8))

struct ma730_config {
	struct spi_dt_spec bus;
	bool reverse;
	uint16_t zero_offset;
};

struct ma730_data {
	uint16_t angle; /* last sample_fetch, reverse and offset applied */
};

/* One 16-bit frame: @p tx out, the answer into @p rx */
static int frame(const struct ma730_config *cfg, uint16_t tx, uint16_t *rx)
{
	uint8_t tx_bytes[2];
	uint8_t rx_bytes[2] = {0};
	const struct spi_buf tx_buf = {.buf = tx_bytes, .len = sizeof(tx_bytes)};
	const struct spi_buf rx_buf = {.buf = rx_bytes, .len = sizeof(rx_bytes)};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};
	int ret;

	sys_put_be16(tx, tx_bytes);
	ret = spi_transceive_dt(&cfg->bus, &tx_set, &rx_set);
	if (ret == 0) {
		*rx = sys_get_be16(rx_bytes);
	}
	return ret;
}

static uint16_t to_angle(const struct ma730_config *cfg, uint16_t raw)
{
	const uint16_t turned = cfg->reverse ? (uint16_t)(0U - raw) : raw;

	return (uint16_t)(turned - cfg->zero_offset);
}

int ma730_read_angle(const struct device *dev, uint16_t *angle)
{
	const struct ma730_config *cfg = dev->config;
	uint16_t raw;
	int ret = frame(cfg, 0x0000U, &raw);

	if (ret == 0) {
		*angle = to_angle(cfg, raw);
	}
	return ret;
}

int ma730_read_reg(const struct device *dev, uint8_t reg, uint8_t *value)
{
	const struct ma730_config *cfg = dev->config;
	uint16_t rx;
	int ret;

	if (reg > 0x1FU) {
		return -EINVAL;
	}
	ret = frame(cfg, MA730_CMD_READ | ((uint16_t)reg << 8), &rx);
	if (ret != 0) {
		return ret;
	}
	k_busy_wait(MA730_IDLE_US);
	ret = frame(cfg, 0x0000U, &rx);
	k_busy_wait(MA730_IDLE_US);
	if (ret == 0) {
		*value = (uint8_t)(rx >> 8);
	}
	return ret;
}

int ma730_read_field(const struct device *dev, bool *high, bool *low)
{
	uint8_t flags;
	int ret = ma730_read_reg(dev, MA730_REG_FIELD, &flags);

	if (ret == 0) {
		*high = (flags & MA730_FIELD_HIGH) != 0U;
		*low = (flags & MA730_FIELD_LOW) != 0U;
	}
	return ret;
}

static int ma730_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct ma730_data *data = dev->data;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_ROTATION)) {
		return -ENOTSUP;
	}
	return ma730_read_angle(dev, &data->angle);
}

static int ma730_channel_get(const struct device *dev, enum sensor_channel chan,
			     struct sensor_value *val)
{
	const struct ma730_data *data = dev->data;
	/* micro-degrees: 360e6 / 65536 per count, below 2^32 for every angle */
	const uint64_t udeg = ((uint64_t)data->angle * 360000000ULL) / MA730_COUNTS_PER_REV;

	if (chan != SENSOR_CHAN_ROTATION) {
		return -ENOTSUP;
	}
	val->val1 = (int32_t)(udeg / 1000000U);
	val->val2 = (int32_t)(udeg % 1000000U);
	return 0;
}

static DEVICE_API(sensor, ma730_api) = {
	.sample_fetch = ma730_sample_fetch,
	.channel_get = ma730_channel_get,
};

/* No SPI traffic here: the MA730 answers valid data 260 ms after power-up, and the
 * application checks the field (ma730_read_field()) once it needs the angle.
 */
static int ma730_init(const struct device *dev)
{
	const struct ma730_config *cfg = dev->config;

	if (!spi_is_ready_dt(&cfg->bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}
	return 0;
}

#define MA730_DEFINE(n)                                                                            \
	static struct ma730_data ma730_data_##n;                                                   \
	static const struct ma730_config ma730_config_##n = {                                      \
		.bus = SPI_DT_SPEC_INST_GET(n, MA730_SPI_OP),                                      \
		.reverse = DT_INST_PROP(n, reverse),                                               \
		.zero_offset = DT_INST_PROP(n, zero_offset),                                       \
	};                                                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(n, ma730_init, NULL, &ma730_data_##n, &ma730_config_##n,      \
				     POST_KERNEL, CONFIG_MA730_INIT_PRIORITY, &ma730_api);

DT_INST_FOREACH_STATUS_OKAY(MA730_DEFINE)
