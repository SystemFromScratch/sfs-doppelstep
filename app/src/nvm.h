/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent blocks of the emulated DYNAMIXEL (EEPROM area, Control Table
 * backup) in the storage_partition, with Zephyr NVS. The X430 library adds
 * the header and the CRC; this layer stores opaque blocks by slot.
 */

#ifndef SFS_NVM_H
#define SFS_NVM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the NVS file system (no-op without CONFIG_SFS_DXL_NVM). */
int nvm_init(void);

/** Read block @p slot; 0 when exactly @p size bytes were read, negative otherwise. */
int nvm_load(uint8_t slot, uint8_t *data, size_t size);

/** Write block @p slot; 0 on success. */
int nvm_store(uint8_t slot, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SFS_NVM_H */
