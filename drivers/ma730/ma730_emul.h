/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test hooks of the MA730 emulator (ma730_emul.c).
 */

#ifndef DOPPELSTEP_DRIVERS_MA730_EMUL_H_
#define DOPPELSTEP_DRIVERS_MA730_EMUL_H_

#include <stdint.h>

#include <zephyr/drivers/emul.h>

/** Raw angle the sensor answers [1/65536 revolution], before reverse and offset. */
void ma730_emul_set_angle(const struct emul *target, uint16_t raw);
/** Register value of a read command. */
void ma730_emul_set_reg(const struct emul *target, uint8_t reg, uint8_t value);
/** SPI frames seen so far. */
uint32_t ma730_emul_frames(const struct emul *target);
/** Write commands seen so far (the driver must send none). */
uint32_t ma730_emul_writes(const struct emul *target);

/** Called after every frame with the frame the driver sent, in the caller's thread. */
typedef void (*ma730_emul_frame_hook_t)(uint16_t tx, void *ctx);
/** Install a frame hook (tests of the frame order between threads); NULL removes it. */
void ma730_emul_set_frame_hook(const struct emul *target, ma730_emul_frame_hook_t hook,
			       void *ctx);

#endif /* DOPPELSTEP_DRIVERS_MA730_EMUL_H_ */
