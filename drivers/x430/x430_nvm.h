/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent blocks of the X430 Control Table (EEPROM area, backup) in NVS on a
 * flash partition. With CONFIG_SFS_DXL_NVM=n or no partition: not persistent.
 */

#ifndef DOPPELSTEP_X430_NVM_H_
#define DOPPELSTEP_X430_NVM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/device.h>
#include <zephyr/kvss/nvs.h>

struct x430_nvm {
	struct nvs_fs fs;
	bool mounted;
};

/** Mount NVS on the partition; @p flash NULL means no storage (-ENODEV). */
int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size);

/** Read block @p slot; 0 when @p size bytes were read, negative otherwise. */
int x430_nvm_load(struct x430_nvm *nvm, uint8_t slot, uint8_t *data, size_t size);

/** Write block @p slot. */
int x430_nvm_store(struct x430_nvm *nvm, uint8_t slot, const uint8_t *data, size_t size);

#endif /* DOPPELSTEP_X430_NVM_H_ */
