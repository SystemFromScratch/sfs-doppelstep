/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPS MA730 magnetic angle sensor (compatible "sfs,ma730"). Besides the sensor API
 * (SENSOR_CHAN_ROTATION in degrees), the raw angle and the field flags.
 */

#ifndef DOPPELSTEP_DRIVERS_MA730_H_
#define DOPPELSTEP_DRIVERS_MA730_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Counts of the raw angle per revolution. */
#define MA730_COUNTS_PER_REV 65536U

/** Register of the magnetic field flags. */
#define MA730_REG_FIELD  0x1BU
#define MA730_FIELD_HIGH BIT(7) /**< MGH: field above the MGHT threshold. */
#define MA730_FIELD_LOW  BIT(6) /**< MGL: field below the MGLT threshold. */

/**
 * Angle [1/65536 revolution], with reverse and zero-offset of the node applied.
 * Any thread; one 16-bit SPI frame.
 */
int ma730_read_angle(const struct device *dev, uint16_t *angle);

/**
 * Read a register (two SPI frames, kept together against other threads). Any thread.
 * Read-only access: the driver has no write.
 */
int ma730_read_reg(const struct device *dev, uint8_t reg, uint8_t *value);

/**
 * Magnetic field flags: @p high above the MGHT threshold, @p low below MGLT. Both
 * false: the magnet is in range. Any thread.
 */
int ma730_read_field(const struct device *dev, bool *high, bool *low);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_DRIVERS_MA730_H_ */
