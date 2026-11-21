/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulator of the MA730 on an emulated SPI bus (tests/drivers/ma730): every
 * spi_transceive() is one frame with its own chip select. A frame answers the angle,
 * or, right after a read command, the register in the upper byte.
 */

#define DT_DRV_COMPAT sfs_ma730

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/sys/byteorder.h>

#include "ma730_emul.h"

struct ma730_emul_data {
	uint16_t angle;
	uint8_t regs[32];
	int pending_reg; /* register of the previous read command, -1 = none */
	uint32_t frames;
	uint32_t writes;
	uint16_t last_tx;
};

void ma730_emul_set_angle(const struct emul *target, uint16_t raw)
{
	struct ma730_emul_data *data = target->data;

	data->angle = raw;
}

void ma730_emul_set_reg(const struct emul *target, uint8_t reg, uint8_t value)
{
	struct ma730_emul_data *data = target->data;

	data->regs[reg & 0x1FU] = value;
}

uint32_t ma730_emul_frames(const struct emul *target)
{
	const struct ma730_emul_data *data = target->data;

	return data->frames;
}

uint32_t ma730_emul_writes(const struct emul *target)
{
	const struct ma730_emul_data *data = target->data;

	return data->writes;
}

static int ma730_emul_io(const struct emul *target, const struct spi_config *config,
			 const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	struct ma730_emul_data *data = target->data;
	uint16_t tx = 0U;
	uint16_t rx;

	ARG_UNUSED(config);

	if ((tx_bufs == NULL) || (tx_bufs->count != 1U) || (tx_bufs->buffers[0].len != 2U)) {
		return -EIO; /* the driver sends single 16-bit frames */
	}
	tx = sys_get_be16(tx_bufs->buffers[0].buf);
	data->last_tx = tx;
	data->frames++;

	if (data->pending_reg >= 0) {
		rx = (uint16_t)data->regs[data->pending_reg] << 8;
		data->pending_reg = -1;
	} else {
		rx = data->angle;
	}
	switch (tx >> 13) {
	case 0x2U: /* 010: read register */
		data->pending_reg = (tx >> 8) & 0x1FU;
		break;
	case 0x4U: /* 100: write register */
		data->writes++;
		break;
	default:
		break;
	}
	if ((rx_bufs != NULL) && (rx_bufs->count == 1U) && (rx_bufs->buffers[0].len == 2U)) {
		sys_put_be16(rx, rx_bufs->buffers[0].buf);
	}
	return 0;
}

static const struct spi_emul_api ma730_emul_api = {
	.io = ma730_emul_io,
};

static int ma730_emul_init(const struct emul *target, const struct device *parent)
{
	struct ma730_emul_data *data = target->data;

	ARG_UNUSED(parent);
	data->pending_reg = -1;
	return 0;
}

#define MA730_EMUL(n)                                                                              \
	static struct ma730_emul_data ma730_emul_data_##n;                                         \
	EMUL_DT_INST_DEFINE(n, ma730_emul_init, &ma730_emul_data_##n, NULL, &ma730_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(MA730_EMUL)
