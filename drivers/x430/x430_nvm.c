/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "x430_nvm.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(x430_nvm, CONFIG_X430_SERVO_LOG_LEVEL);

uint16_t x430_nvm_block_size(uint8_t slot)
{
	return (slot == (uint8_t)X430_NVM_BACKUP) ? (uint16_t)X430_NVM_BACKUP_SIZE
						 : (uint16_t)X430_NVM_EEPROM_SIZE;
}

#if defined(CONFIG_SFS_DXL_NVM)

#include <zephyr/drivers/flash.h>

/* NVS id = slot + NVM_ID_BASE. Firmware up to 0.2.0 (emulator before v1.0.0) stored
 * blocks without header and CRC under ids 1 and 2: a new id base makes such a block
 * "missing", so the board starts with the factory values of this firmware once, and
 * the old items are deleted.
 */
#define NVM_ID_BASE        0x11U
#define NVM_ID_OLD_FIRST   1U
#define NVM_ID_OLD_LAST    2U

int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size)
{
	struct flash_pages_info page;
	int ret;

	nvm->cached_slot = -1;
	nvm->write_slot = -1;
	if ((flash == NULL) || !device_is_ready(flash)) {
		return -ENODEV;
	}
	ret = flash_get_page_info_by_offs(flash, offset, &page);
	if (ret != 0) {
		return ret;
	}
	nvm->fs.flash_device = flash;
	nvm->fs.offset = offset;
	nvm->fs.sector_size = page.size;
	nvm->fs.sector_count = (uint16_t)(size / page.size);

	ret = nvs_mount(&nvm->fs);
	if (ret != 0) {
		LOG_ERR("nvs_mount: %d", ret);
		return ret;
	}
	nvm->mounted = true;
	LOG_INF("NVS: %u sectors of %u bytes", nvm->fs.sector_count, nvm->fs.sector_size);

	for (uint16_t id = NVM_ID_OLD_FIRST; id <= NVM_ID_OLD_LAST; id++) {
		if (nvs_read(&nvm->fs, id, nvm->buf, 1U) > 0) {
			LOG_INF("NVS: block %u of an older firmware deleted (factory values)", id);
			(void)nvs_delete(&nvm->fs, id);
		}
	}
	return 0;
}

int x430_nvm_read(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, uint8_t *data,
		  uint16_t length)
{
	const uint16_t size = x430_nvm_block_size(slot);

	if (!nvm->mounted) {
		return -ENODEV;
	}
	if (((uint32_t)offset + length) > size) {
		return -EINVAL;
	}
	if ((nvm->cached_slot != (int)slot) || (nvm->write_slot >= 0)) {
		const ssize_t rc = nvs_read(&nvm->fs, (uint16_t)(slot + NVM_ID_BASE), nvm->buf, size);

		nvm->write_slot = -1;
		if (rc != (ssize_t)size) {
			nvm->cached_slot = -1;
			return -ENOENT;
		}
		nvm->cached_slot = (int)slot;
	}
	memcpy(data, &nvm->buf[offset], length);
	return 0;
}

int x430_nvm_write(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, const uint8_t *data,
		   uint16_t length)
{
	const uint16_t size = x430_nvm_block_size(slot);
	ssize_t rc;

	if (!nvm->mounted) {
		return -ENODEV;
	}
	if (offset == 0U) {
		nvm->write_slot = (int)slot; /* a new block */
		nvm->write_end = 0U;
		nvm->cached_slot = -1;
	}
	if ((nvm->write_slot != (int)slot) || (offset != nvm->write_end) ||
	    (((uint32_t)offset + length) > size)) {
		nvm->write_slot = -1;
		LOG_ERR("NVS slot %u: part at %u out of order", slot, offset);
		return -EINVAL;
	}
	memcpy(&nvm->buf[offset], data, length);
	nvm->write_end = (uint16_t)(offset + length);
	if (nvm->write_end < size) {
		return 0;
	}

	nvm->write_slot = -1;
	rc = nvs_write(&nvm->fs, (uint16_t)(slot + NVM_ID_BASE), nvm->buf, size);
	if (rc < 0) {
		LOG_ERR("nvs_write slot %u: %d", slot, (int)rc);
		return (int)rc;
	}
	nvm->cached_slot = (int)slot; /* the buffer holds what is stored now */
	return 0;
}

#else /* CONFIG_SFS_DXL_NVM */

int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size)
{
	ARG_UNUSED(flash);
	ARG_UNUSED(offset);
	ARG_UNUSED(size);
	nvm->mounted = false;
	nvm->cached_slot = -1;
	nvm->write_slot = -1;
	return 0;
}

int x430_nvm_read(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, uint8_t *data,
		  uint16_t length)
{
	ARG_UNUSED(nvm);
	ARG_UNUSED(slot);
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(length);
	return -ENOTSUP;
}

int x430_nvm_write(struct x430_nvm *nvm, uint8_t slot, uint16_t offset, const uint8_t *data,
		   uint16_t length)
{
	ARG_UNUSED(nvm);
	ARG_UNUSED(slot);
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(length);
	return -ENOTSUP;
}

#endif /* CONFIG_SFS_DXL_NVM */
