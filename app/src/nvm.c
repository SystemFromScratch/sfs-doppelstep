/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS on the storage_partition (2 x 2 KiB pages on B-G431B-ESC1).
 */

#include "nvm.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nvm, CONFIG_SFS_LOG_LEVEL);

#if defined(CONFIG_SFS_DXL_NVM)

#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#define NVM_PARTITION storage_partition
#define NVM_ID_BASE 1U /* NVS id = slot + NVM_ID_BASE */

static struct nvs_fs fs;
static bool mounted;

int nvm_init(void)
{
	struct flash_pages_info page;
	int ret;

	fs.flash_device = PARTITION_DEVICE(NVM_PARTITION);
	if (!device_is_ready(fs.flash_device)) {
		return -ENODEV;
	}
	fs.offset = PARTITION_OFFSET(NVM_PARTITION);
	ret = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page);
	if (ret != 0) {
		return ret;
	}
	fs.sector_size = page.size;
	fs.sector_count = (uint16_t)(PARTITION_SIZE(NVM_PARTITION) / page.size);

	ret = nvs_mount(&fs);
	if (ret != 0) {
		LOG_ERR("nvs_mount: %d", ret);
		return ret;
	}
	mounted = true;
	LOG_INF("NVS: %u sectors of %u bytes", fs.sector_count, fs.sector_size);
	return 0;
}

int nvm_load(uint8_t slot, uint8_t *data, size_t size)
{
	ssize_t rc;

	if (!mounted) {
		return -ENODEV;
	}
	rc = nvs_read(&fs, (uint16_t)(slot + NVM_ID_BASE), data, size);
	return (rc == (ssize_t)size) ? 0 : -ENOENT;
}

int nvm_store(uint8_t slot, const uint8_t *data, size_t size)
{
	ssize_t rc;

	if (!mounted) {
		return -ENODEV;
	}
	rc = nvs_write(&fs, (uint16_t)(slot + NVM_ID_BASE), data, size);
	if (rc < 0) {
		LOG_ERR("nvs_write slot %u: %d", slot, (int)rc);
		return (int)rc;
	}
	return 0;
}

#else /* CONFIG_SFS_DXL_NVM */

int nvm_init(void)
{
	return 0;
}

int nvm_load(uint8_t slot, uint8_t *data, size_t size)
{
	ARG_UNUSED(slot);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENOTSUP;
}

int nvm_store(uint8_t slot, const uint8_t *data, size_t size)
{
	ARG_UNUSED(slot);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENOTSUP;
}

#endif /* CONFIG_SFS_DXL_NVM */
