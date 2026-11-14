/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent blocks of the X430 Control Table (EEPROM area, backup) in NVS on a
 * flash partition. With CONFIG_SFS_DXL_NVM=n or no partition: not persistent.
 *
 * The emulator (DYNAMIXEL-X430 v1.0.0) reads and writes a block in parts at an
 * offset (nvm_read / nvm_write hooks). An NVS item is read and written whole, so one
 * block lives in a RAM buffer here: a read loads the item once and serves the parts
 * from the buffer; a write collects the parts from offset 0 and stores the item when
 * the last part reaches the size of the block.
 */

#ifndef DOPPELSTEP_X430_NVM_H_
#define DOPPELSTEP_X430_NVM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/device.h>
#include <zephyr/kvss/nvs.h>

#include <x430.h>

/** Largest persistent block of the emulator */
#define X430_NVM_BLOCK_MAX                                                                         \
	((X430_NVM_BACKUP_SIZE > X430_NVM_EEPROM_SIZE) ? X430_NVM_BACKUP_SIZE : X430_NVM_EEPROM_SIZE)

struct x430_nvm {
	struct nvs_fs fs;
	bool mounted;
	/* One block: loaded for reads (cached_slot) or collected for a write */
	uint8_t buf[X430_NVM_BLOCK_MAX];
	int cached_slot; /* slot whose item is in buf, -1 = none */
	int write_slot;  /* slot being collected, -1 = none */
	uint16_t write_end;
};

/** Size of the block @p slot (X430_NVM_EEPROM_SIZE or X430_NVM_BACKUP_SIZE). */
uint16_t x430_nvm_block_size(uint8_t slot);

/** Mount NVS on the partition; @p flash NULL means no storage (-ENODEV). */
int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size);

/** Read @p length bytes of block @p slot at @p offset; 0, or negative when there is
 *  no stored block of the right size (the emulator then keeps its factory values).
 */
int x430_nvm_read(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, uint8_t *data,
		  uint16_t length);

/** Write part of block @p slot; offset 0 starts the block, the part that reaches its
 *  size stores it. 0, or negative (parts out of order, flash error).
 */
int x430_nvm_write(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, const uint8_t *data,
		   uint16_t length);

#endif /* DOPPELSTEP_X430_NVM_H_ */
